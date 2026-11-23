# Keep the native, PE, Autoconf and Meson builds in their own toolchains.
# ExternalProject dependencies order *configuration*, not just final linking.
include(ExternalProject)
include("${CMAKE_CURRENT_LIST_DIR}/DependencySources.cmake")
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$"
        OR CMAKE_CROSSCOMPILING OR CMAKE_CONFIGURATION_TYPES OR CMAKE_BINARY_DIR STREQUAL CMAKE_SOURCE_DIR)
    message(FATAL_ERROR "Use a separate build directory and a native, single-configuration x86-64 Linux generator (cmake --preset runtime).")
endif()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type for the Frosty tools and guest" FORCE)
endif()

find_package(Git REQUIRED)
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
find_package(PkgConfig REQUIRED)
find_program(MESON meson REQUIRED)
execute_process(COMMAND "${MESON}" --version OUTPUT_VARIABLE meson_version OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(meson_version VERSION_LESS 1.1)
    message(FATAL_ERROR "Meson 1.1 or newer is required (found ${meson_version})")
endif()
find_program(NINJA ninja REQUIRED)
find_program(GNU_MAKE NAMES gmake make REQUIRED)
find_program(MINGW_CC x86_64-w64-mingw32-gcc REQUIRED)
find_program(MINGW_CXX x86_64-w64-mingw32-g++ REQUIRED)
find_program(WIDL NAMES widl widl-stable x86_64-w64-mingw32-widl REQUIRED)
find_program(GLSLANG NAMES glslangValidator glslang REQUIRED)
find_program(NASM nasm REQUIRED)
find_program(BISON bison REQUIRED)
find_program(FLEX flex REQUIRED)

set(root "${CMAKE_BINARY_DIR}")
set(native "${root}/deps/native")
set(dxc "${root}/deps/dxc")
set(audio "${root}/deps/audio-win64")
set(toolchain "${CMAKE_SOURCE_DIR}/cmake/mingw64.cmake")
set_property(DIRECTORY PROPERTY EP_BASE "${root}/_ep")
set(FROSTY_JOBS 4 CACHE STRING "Maximum parallel compiler jobs per dependency")
if(NOT FROSTY_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "FROSTY_JOBS must be a positive integer")
endif()
set(child_build "${CMAKE_COMMAND}" --build <BINARY_DIR> --parallel "${FROSTY_JOBS}")
set(native_args "-DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}" "-DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}"
    "-DCMAKE_INSTALL_PREFIX:PATH=${native}" -DCMAKE_INSTALL_LIBDIR:STRING=lib -DCMAKE_BUILD_TYPE:STRING=Release)

# A hand-built tree may already be the user's running copy. Adopt the source
# recorded by its own build system, just as an existing CMake cache would.
function(frosty_previous_source id out)
    set(previous "")
    if(id STREQUAL "wine" AND EXISTS "${root}/wine64/Makefile")
        file(STRINGS "${root}/wine64/Makefile" line REGEX "^srcdir = " LIMIT_COUNT 1)
        if(line)
            string(REGEX REPLACE "^srcdir = " "" path "${line}")
            file(REAL_PATH "${path}" previous BASE_DIRECTORY "${root}/wine64")
        endif()
    elseif(id MATCHES "^(dxvk|vkd3d)$" AND EXISTS "${root}/${id}/meson-info/meson-info.json")
        file(READ "${root}/${id}/meson-info/meson-info.json" info)
        string(JSON previous GET "${info}" directories source)
    elseif(id STREQUAL "grt" AND EXISTS "${root}/grt/CMakeCache.txt")
        file(STRINGS "${root}/grt/CMakeCache.txt" line REGEX "^CMAKE_HOME_DIRECTORY:INTERNAL=")
        string(REPLACE "CMAKE_HOME_DIRECTORY:INTERNAL=" "" previous "${line}")
    elseif(id STREQUAL "ffmpeg" AND IS_SYMLINK "${root}/audio-ffmpeg/xs-source/configure")
        file(REAL_PATH "${root}/audio-ffmpeg/xs-source/configure" config)
        get_filename_component(previous "${config}" DIRECTORY)
    endif()
    set(${out} "${previous}" PARENT_SCOPE)
endfunction()

# Select either an immutable upstream + our patches, a published fork, or a
# developer's existing worktree. Never run git commands on the latter.
function(frosty_input id marker)
    string(TOUPPER "${id}" name)
    set(var "FROSTY_${name}_SOURCE")
    set(source "${root}/_sources/${id}")
    frosty_previous_source("${id}" previous)
    set(default "")
    if(previous AND NOT previous STREQUAL source)
        set(default "${previous}")
        if(NOT DEFINED ${var})
            message(STATUS "Reusing the existing ${id} build's source: ${previous}")
        endif()
    endif()
    set(${var} "${default}" CACHE PATH "Use this prepared ${id} source/distribution in place of fetching it")
    set(fetch ${${id}_download})
    set(packaged TRUE)
    if(id MATCHES "^(wine|vkd3d)$")
        set(repo "FROSTY_${name}_REPOSITORY")
        set(rev "FROSTY_${name}_REVISION")
        set(${repo} "" CACHE STRING "Published ${id} fork, with the Frosty changes committed")
        set(${rev} "" CACHE STRING "Full commit SHA in the published ${id} fork")
        if(${repo} OR ${rev})
            if(${var} OR NOT ${repo} OR NOT ${rev})
                message(FATAL_ERROR "Choose either ${var}, or both ${repo} and ${rev}")
            endif()
            frosty_git(fetch "${${repo}}" "${${rev}}")
            set(packaged FALSE)
        endif()
    endif()
    if(${var})
        file(REAL_PATH "${${var}}" source)
        if(NOT EXISTS "${source}/${marker}")
            message(FATAL_ERROR "${var} is missing ${marker}: ${source}")
        endif()
        set(fetch)
        set(packaged FALSE)
    endif()
    if(previous AND NOT previous STREQUAL source)
        message(FATAL_ERROR "The existing ${id} build uses ${previous}. Use a fresh -B directory to switch its source.")
    endif()
    set(${id}_src "${source}" PARENT_SCOPE)
    set(${id}_input SOURCE_DIR "${source}" ${fetch} PARENT_SCOPE)
    set(${id}_packaged "${packaged}" PARENT_SCOPE)
endfunction()

function(frosty_patches id)
    if(${id}_packaged)
        set(script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ApplyRuntimePatches.cmake")
        set(files "${script}")
        foreach(name IN LISTS ARGN)
            list(APPEND files "${CMAKE_SOURCE_DIR}/patches/${name}")
        endforeach()
        ExternalProject_Add_Step(${id} frosty-patches
            COMMAND "${CMAKE_COMMAND}" "-DGIT=${GIT_EXECUTABLE}" "-DSOURCE=<SOURCE_DIR>" "-DCOMPONENT=${id}" -P "${script}"
            DEPENDS ${files} DEPENDEES patch DEPENDERS configure INDEPENDENT TRUE)
    endif()
endfunction()

frosty_input(zlib zlib.h)
ExternalProject_Add(zlib ${zlib_input} CMAKE_ARGS ${native_args}
    -DZLIB_BUILD_TESTING=OFF -DZLIB_BUILD_SHARED=OFF BUILD_COMMAND ${child_build})
frosty_input(lz4 lib/lz4.h)
ExternalProject_Add(lz4 ${lz4_input} SOURCE_SUBDIR build/cmake CMAKE_ARGS ${native_args}
    -DLZ4_BUILD_CLI=OFF -DBUILD_SHARED_LIBS=OFF BUILD_COMMAND ${child_build})

frosty_input(dxc include/dxc/dxcapi.h)
if(dxc_src STREQUAL dxc)
    add_custom_target(host-dxc)
else()
    ExternalProject_Add(host-dxc ${dxc_input} CONFIGURE_COMMAND "" BUILD_COMMAND ""
        INSTALL_COMMAND "${CMAKE_COMMAND}" -E make_directory "${dxc}/bin" "${dxc}/lib"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory <SOURCE_DIR>/include/dxc "${dxc}/include/dxc"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different <SOURCE_DIR>/bin/dxc "${dxc}/bin/"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different <SOURCE_DIR>/lib/libdxcompiler.so <SOURCE_DIR>/lib/libdxil.so "${dxc}/lib/"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different <SOURCE_DIR>/LICENSE-LLVM.txt <SOURCE_DIR>/LICENSE-MS.txt "${dxc}/")
endif()

# Use the system SDKs when available. On a machine without them, build the
# pinned native libraries; the Vulkan driver/ICD is still supplied by Linux.
find_package(LLVM QUIET CONFIG)
set(need_llvm ON)
if(TARGET LLVM AND "AMDGPU" IN_LIST LLVM_TARGETS_TO_BUILD AND LLVM_VERSION_MAJOR EQUAL 22)
    set(need_llvm OFF)
endif()
option(FROSTY_BUILD_LLVM "Build pinned shared LLVM with the AMDGPU disassembler" ${need_llvm})
find_package(Vulkan QUIET)
set(need_vulkan ON)
if(Vulkan_FOUND)
    set(need_vulkan OFF)
endif()
option(FROSTY_BUILD_VULKAN "Build pinned native Vulkan headers and loader" ${need_vulkan})
set(host_deps zlib lz4 host-dxc)
if(FROSTY_BUILD_LLVM)
    frosty_input(llvm llvm/CMakeLists.txt)
    ExternalProject_Add(host-llvm ${llvm_input} SOURCE_SUBDIR llvm CMAKE_ARGS ${native_args}
        -DLLVM_TARGETS_TO_BUILD=AMDGPU -DLLVM_BUILD_LLVM_DYLIB=ON -DLLVM_LINK_LLVM_DYLIB=ON
        -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF
        -DLLVM_BUILD_TOOLS=OFF -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF
        BUILD_COMMAND ${child_build})
    list(APPEND host_deps host-llvm)
    set(llvm_dir "${native}/lib/cmake/llvm")
elseif(NOT TARGET LLVM OR NOT "AMDGPU" IN_LIST LLVM_TARGETS_TO_BUILD)
    message(FATAL_ERROR "Cannot find shared LLVM with AMDGPU. Set LLVM_DIR or enable FROSTY_BUILD_LLVM.")
else()
    set(llvm_dir "${LLVM_DIR}")
endif()
if(FROSTY_BUILD_VULKAN)
    frosty_input(vkheaders include/vulkan/vulkan.h)
    ExternalProject_Add(vk-headers ${vkheaders_input} CMAKE_ARGS ${native_args}
        -DVULKAN_HEADERS_ENABLE_TESTS=OFF BUILD_COMMAND ${child_build})
    frosty_input(vkloader loader/CMakeLists.txt)
    ExternalProject_Add(vk-loader ${vkloader_input} DEPENDS vk-headers CMAKE_ARGS ${native_args}
        "-DCMAKE_PREFIX_PATH:PATH=${native}" -DUPDATE_DEPS=OFF -DBUILD_TESTS=OFF BUILD_COMMAND ${child_build})
    list(APPEND host_deps vk-loader)
    set(vk_include "${native}/include")
    set(vk_library "${native}/lib/libvulkan.so")
elseif(NOT Vulkan_FOUND)
    message(FATAL_ERROR "Install Vulkan development files or enable FROSTY_BUILD_VULKAN.")
else()
    set(vk_include "${Vulkan_INCLUDE_DIR}")
    set(vk_library "${Vulkan_LIBRARY}")
endif()

option(FROSTY_BUILD_INSTALLER "Build the native Linux installer and enable the ZIP release target" ON)
ExternalProject_Add(frosty-host SOURCE_DIR "${CMAKE_SOURCE_DIR}" BINARY_DIR "${root}/host"
    DOWNLOAD_COMMAND "" DEPENDS ${host_deps} CMAKE_ARGS -DFROSTY_SUPERBUILD=OFF
    "-DFROSTY_BUILD_INSTALLER:BOOL=${FROSTY_BUILD_INSTALLER}"
    "-DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}" "-DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}"
    "-DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}" "-DCMAKE_PREFIX_PATH:PATH=${native}"
    "-DZLIB_INCLUDE_DIR:PATH=${native}/include" "-DZLIB_LIBRARY_RELEASE:FILEPATH=${native}/lib/libz.a"
    "-Dlz4_DIR:PATH=${native}/lib/cmake/lz4"
    "-DLLVM_DIR:PATH=${llvm_dir}" "-DDXC_ROOT:PATH=${dxc}"
    "-DDXC_INCLUDE:PATH=${dxc}/include" "-DDXC_LIBRARY:FILEPATH=${dxc}/lib/libdxcompiler.so"
    "-DVulkan_INCLUDE_DIR:PATH=${vk_include}" "-DVulkan_LIBRARY:FILEPATH=${vk_library}"
    BUILD_ALWAYS TRUE BUILD_COMMAND ${child_build} INSTALL_COMMAND "")

frosty_input(wine configure)
set(wine_env "CC=${CMAKE_C_COMPILER}" "x86_64_CC=${MINGW_CC}")
if(NOT DEFINED ENV{CFLAGS})
    list(APPEND wine_env "CFLAGS=-O2 -g0")
endif()
if(NOT DEFINED ENV{CROSSCFLAGS})
    list(APPEND wine_env "CROSSCFLAGS=-O2 -g0")
endif()
set(wine_deps)
if(FROSTY_BUILD_VULKAN)
    list(APPEND wine_deps vk-loader)
    list(APPEND wine_env "CPPFLAGS=-I${native}/include $ENV{CPPFLAGS}" "LDFLAGS=-L${native}/lib $ENV{LDFLAGS}")
endif()
ExternalProject_Add(wine ${wine_input} BINARY_DIR "${root}/wine64" DEPENDS ${wine_deps}
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env ${wine_env} <SOURCE_DIR>/configure
        --enable-archs=x86_64 --disable-tests --with-x --with-vulkan --with-sdl --with-udev --with-gstreamer --with-pulse
    BUILD_COMMAND "${GNU_MAKE}" "-j${FROSTY_JOBS}" all include/gameinput.h
    BUILD_ALWAYS TRUE INSTALL_COMMAND "")
frosty_patches(wine wine-runtime.patch wine-mapped-dualsense.patch)

function(frosty_meson id)
    frosty_input(${id} build-win64.txt)
    set(${id}_src "${${id}_src}" PARENT_SCOPE)
    ExternalProject_Add(${id} ${${id}_input} BINARY_DIR "${root}/${id}"
        CONFIGURE_COMMAND "${MESON}" setup --reconfigure --buildtype release --wrap-mode=nodownload
            --cross-file <SOURCE_DIR>/build-win64.txt ${ARGN} <BINARY_DIR> <SOURCE_DIR>
        BUILD_COMMAND "${NINJA}" -C <BINARY_DIR> "-j${FROSTY_JOBS}" ${${id}_targets}
        BUILD_ALWAYS TRUE INSTALL_COMMAND "")
    if(id STREQUAL "vkd3d")
        frosty_patches(vkd3d vkd3d-runtime.patch vkd3d-installer-cache.patch dxil-spirv-subgroup.patch)
    endif()
endfunction()
set(vkd3d_targets libs/d3d12/d3d12.dll libs/d3d12core/d3d12core.dll)
frosty_meson(vkd3d)
set(dxvk_targets src/dxgi/dxgi.dll)
frosty_meson(dxvk -Denable_d3d8=false -Denable_d3d9=false -Denable_d3d10=false -Denable_d3d11=false)

frosty_input(grt CMakeLists.txt)
ExternalProject_Add(grt ${grt_input} BINARY_DIR "${root}/grt" CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=<SOURCE_DIR>/win64-target.cmake"
    "-DWIDL:FILEPATH=${WIDL}" -DENABLE_TESTS=OFF
    BUILD_COMMAND ${child_build} --target xgameruntime BUILD_ALWAYS TRUE INSTALL_COMMAND "")

frosty_input(ffmpeg configure)
set(codec_script "${CMAKE_SOURCE_DIR}/tools/build_audio_codec.py")
ExternalProject_Add(audio-codec ${ffmpeg_input} BINARY_DIR "${root}/audio-ffmpeg" CONFIGURE_COMMAND ""
    BUILD_COMMAND "${Python3_EXECUTABLE}" "${codec_script}" <SOURCE_DIR>
        --build-dir <BINARY_DIR> --prefix "${audio}" --jobs "${FROSTY_JOBS}"
    BUILD_BYPRODUCTS "${audio}/lib/libavcodec.dll.a" "${audio}/lib/libavutil.dll.a"
        "${audio}/bin/avcodec-62.dll" "${audio}/bin/avutil-60.dll"
    INSTALL_COMMAND "")
ExternalProject_Add_Step(audio-codec adapter-input DEPENDEES configure DEPENDERS build
    DEPENDS "${codec_script}" "${CMAKE_SOURCE_DIR}/audio/ffmpeg/xma2frame.inc")

frosty_input(libdeflate libdeflate.h)
ExternalProject_Add(libdeflate-source ${libdeflate_input} CONFIGURE_COMMAND "" BUILD_COMMAND "" INSTALL_COMMAND "")
ExternalProject_Add(frosty-guest SOURCE_DIR "${CMAKE_SOURCE_DIR}/guest" BINARY_DIR "${root}/guest"
    DOWNLOAD_COMMAND "" DEPENDS wine audio-codec libdeflate-source CMAKE_ARGS
    "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${toolchain}" "-DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}"
    -DFROSTY_COMPLETE_GUEST=ON "-DFROSTY_WINE_SOURCE:PATH=${wine_src}"
    "-DXS_GAMEINPUT_HEADER:FILEPATH=${root}/wine64/include/gameinput.h"
    "-DFROSTY_LIBDEFLATE_SOURCE:PATH=${libdeflate_src}" "-DXS_AUDIO_FFMPEG:PATH=${audio}"
    "-DFROSTY_GLSLANG:FILEPATH=${GLSLANG}"
    BUILD_ALWAYS TRUE BUILD_COMMAND ${child_build} INSTALL_COMMAND "")

add_custom_target(frosty-runtime ALL
    DEPENDS frosty-host frosty-guest wine vkd3d dxvk grt)
if(FROSTY_BUILD_INSTALLER)
    add_custom_target(frostybird-zip
        COMMAND bash "${CMAKE_SOURCE_DIR}/installer/package.sh" "${CMAKE_SOURCE_DIR}" "${root}" "${wine_src}" "${vkd3d_src}" "${dxvk_src}" "${grt_src}" "${ffmpeg_src}"
        DEPENDS frosty-runtime USES_TERMINAL VERBATIM)
endif()
message(STATUS "Runtime root: ${root} (build LLVM: ${FROSTY_BUILD_LLVM}, build Vulkan SDK libraries: ${FROSTY_BUILD_VULKAN})")

find_package(SDL3 3.2 REQUIRED CONFIG)
find_package(PkgConfig REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED CONFIG)
find_package(Threads REQUIRED)
pkg_check_modules(UI_TEXT REQUIRED IMPORTED_TARGET pangocairo)
pkg_check_modules(VORBISFILE REQUIRED IMPORTED_TARGET vorbisfile)
find_program(UI_GLSLANG NAMES glslangValidator glslang REQUIRED)
foreach(shader screen.vert matrix.frag)
    string(REPLACE "." "_" symbol "${shader}")
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${symbol}.h"
        COMMAND "${UI_GLSLANG}" -V --vn "${symbol}" -o "${CMAKE_CURRENT_BINARY_DIR}/${symbol}.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/${shader}" DEPENDS "ui/${shader}" VERBATIM)
endforeach()
add_library(frostybird-core STATIC files.cpp workload.cpp compiler.cpp)
target_link_libraries(frostybird-core PUBLIC frosty-translation nlohmann_json::nlohmann_json Threads::Threads)
target_include_directories(frostybird-core SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
add_executable(frostybird-profile export.cpp)
target_link_libraries(frostybird-profile PRIVATE frostybird-core)
# The downloaded entry point must start even if the distribution's glibc is
# older than the compiler/build machine's. It runs our ELF with its matching
# private ELF loader, without changing the desktop's library environment.
add_executable(frostybird-start start.cpp)
target_link_options(frostybird-start PRIVATE -static)
add_executable(frostybird main.cpp runtime.cpp service.cpp ui/wizard.cpp ui/graphics.cpp ui/music.cpp ui/cues.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/screen_vert.h" "${CMAKE_CURRENT_BINARY_DIR}/matrix_frag.h")
target_include_directories(frostybird PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(frostybird PRIVATE frostybird-core SDL3::SDL3 PkgConfig::UI_TEXT PkgConfig::VORBISFILE)
foreach(t frostybird-core frostybird-profile frostybird)
    target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()
add_executable(frostybird-cue-check EXCLUDE_FROM_ALL ../tests/installer_cues.cpp ui/cues.cpp)
target_include_directories(frostybird-cue-check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ui")
target_link_libraries(frostybird-cue-check PRIVATE SDL3::SDL3)

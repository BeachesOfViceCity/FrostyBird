# Download steps only populate directories owned by the superbuild. A source
# override is an already-prepared checkout: never fetch, reset or patch it.
set(FROSTY_DISTFILES "" CACHE PATH "Optional directory of predownloaded archives (same filenames and SHA-256 checks)")

function(frosty_archive out filename url sha256)
    if(FROSTY_DISTFILES AND EXISTS "${FROSTY_DISTFILES}/${filename}")
        set(url "${FROSTY_DISTFILES}/${filename}")
    endif()
    set(${out} URL "${url}" DOWNLOAD_NAME "${filename}" URL_HASH "SHA256=${sha256}"
        TLS_VERIFY TRUE PARENT_SCOPE)
endfunction()

function(frosty_git out url revision)
    string(LENGTH "${revision}" length)
    if(NOT length EQUAL 40 OR NOT revision MATCHES "^[0-9a-fA-F]+$")
        message(FATAL_ERROR "Supply a full commit SHA for ${url}, not a moving branch.")
    endif()
    # An unchanged commit needs no fetch; a new pin must be allowed to fetch
    # rather than reusing an old disconnected checkout.
    set(${out} GIT_REPOSITORY "${url}" GIT_TAG "${revision}" GIT_SUBMODULES_RECURSE TRUE PARENT_SCOPE)
endfunction()

set(wine_base 97027f879ba9a36dbe594720d4710818536e5433)
frosty_archive(wine_download "wine-${wine_base}.tar.gz"
    "https://codeload.github.com/wine-mirror/wine/tar.gz/${wine_base}"
    1c217ec6ea372832b01cc11820c2b728e50d17e211e1b5656695150d23bd27a5)
frosty_git(vkd3d_download https://github.com/HansKristian-Work/vkd3d-proton.git cae8353ed7959c448ac976342052de1ec2b81715)
frosty_git(dxvk_download https://github.com/doitsujin/dxvk.git a78a467cadb22bee928904290cf8e7921b121851)
set(grt_revision 642e41c7617310c5d1b39d4e525b01ecae261f9e)
frosty_archive(grt_download "xgameruntime-${grt_revision}.tar.gz"
    "https://codeload.github.com/xodus-gaming/xgameruntime/tar.gz/${grt_revision}"
    f2080fba6ff74995479d5925941a150ea4b35b380c419c564d3d6a2f674dadf6)
frosty_archive(dxc_download linux_dxc_2025_07_14.x86_64.tar.gz
    https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2505.1/linux_dxc_2025_07_14.x86_64.tar.gz
    f2213da1fc99dc8778c8823078e16ba97c7f80f86a1d4520ab1adf4b462bc48c)
frosty_archive(ffmpeg_download ffmpeg-8.1.2.tar.xz https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz
    464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c)
frosty_archive(libdeflate_download libdeflate-1.25.tar.gz https://github.com/ebiggers/libdeflate/archive/refs/tags/v1.25.tar.gz
    d11473c1ad4c57d874695e8026865e38b47116bbcb872bfc622ec8f37a86017d)
frosty_archive(zlib_download zlib-1.3.2.tar.xz https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.xz
    d7a0654783a4da529d1bb793b7ad9c3318020af77667bcae35f95d0e42a792f3)
frosty_archive(lz4_download lz4-1.10.0.tar.gz https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz
    537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b)
frosty_archive(llvm_download llvm-project-22.1.8.src.tar.xz
    https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.8/llvm-project-22.1.8.src.tar.xz
    922f1817a0df7b1489272d18134ee0087a8b068828f87ac63b9861b1a9965888)
frosty_archive(vkheaders_download vulkan-headers-1.4.341.0.tar.gz
    https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.341.0.tar.gz
    d73bc5036b6556b741f6985ff600ca720308c5f2850e4a43ceb498bd3de069e7)
frosty_archive(vkloader_download vulkan-loader-1.4.341.0.tar.gz
    https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/vulkan-sdk-1.4.341.0.tar.gz
    fe982697c780a950641bfcf94707135c26c501352242d285fa95d087d691292e)

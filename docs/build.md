# Building Frosty

## Prerequisites

Use a native x86-64 Linux host. Install:

- CMake 3.22.1+, Ninja, Meson 1.1+, GNU Make, Git, pkg-config, Python 3.11+,
  Flex, Bison, NASM, and glslang with Vulkan 1.3 support.
- Native C and C++23 compilers and a C++23 standard library with `std::format`.
  For the PE build, install the x86-64 MinGW-w64 GCC/G++ toolchain with thread
  support, binutils, Windows headers (including `d3d12.h` and `dxcapi.h`), and
  `widl` with standard Windows IDL includes. The tested compilers were GCC 15.3
  (native) and MinGW GCC 15.2; Wine's WIDL 11.0 worked.
- Wine's Linux build dependencies. Install your distribution's Wine build
  prerequisites, including the development packages for X11/Xext, FreeType,
  SDL2, udev, GStreamer 1.0 core/base (audio, video, tag), and PulseAudio.
  Movie audio also needs a GStreamer AAC decoder at runtime (`faad` was tested).
- A working 64-bit Vulkan driver for your GPU. The build can supply the Vulkan
  headers/loader, but the driver and desktop/audio services come from the OS.

## Build and run

```sh
cmake --preset runtime
cmake --build --preset runtime
```

No `sudo`, Steam installation, Unreal Engine checkout, or game files are used by
the build. The first build fetches the dependencies. Source archives and the
Linux DXC release are SHA-256 checked; Git dependencies and their submodules
use exact commits. [DependencySources.cmake](../cmake/DependencySources.cmake)
contains the URLs, revisions and checksums.

| Build output | Used for |
| --- | --- |
| `build/host/` | C++23 shader translator and material indexer, linked with zlib, LZ4, LLVM, DXC and Vulkan |
| `build/wine64/` | Patched x86-64 Wine loader, Wine DLLs, FAudio, WineBus and generated GameInput header |
| `build/vkd3d/libs/` | `d3d12.dll` and `d3d12core.dll`, including the patched `dxil-spirv` |
| `build/dxvk/src/dxgi/dxgi.dll` | 64-bit DXGI, built from stock DXVK; preferred over the historical Steam fallback |
| `build/grt/bin/xgameruntime.dll` | Stock Xodus Gaming Runtime core used by the local-player provider |
| `build/deps/dxc/`, `build/deps/audio-win64/` | Linux DXC distribution and MinGW FFmpeg 8.1.2 with Frosty's XMA2 adapter |
| `build/guest/` | Xbox API bridge DLLs and checks, including `AcpHal.dll`, both shared FFmpeg DLLs, and `xs-controller-check.exe` |

The full build orders Wine and the codec **before configuring the guest** so a
clean build cannot silently skip the audio, controller check or Vulkan ray-row
targets. It cross-builds libdeflate into the storage provider. By default it uses
an installed shared LLVM 22 with AMDGPU and the installed Vulkan SDK libraries
when available. To build the pinned LLVM and Vulkan libraries as well:

```sh
cmake --preset runtime -DFROSTY_BUILD_LLVM=ON -DFROSTY_BUILD_VULKAN=ON
cmake --build --preset runtime
```

The preset runs two dependency builds at a time, with four compiler jobs each.
Use `-DFROSTY_JOBS=N` at configure time and `--parallel N` on the outer build
to adjust that. For host-only development, use `-DFROSTY_SUPERBUILD=OFF` in a
separate binary directory and supply the host libraries and `DXC_ROOT`.

### First launch

Point the launcher at your original installed package. It needs `Mount/` with
the shipping executable and cooked content, and the **sibling**
`EmbeddedXvd/Windows/System32/` with the package's XS compiler, XG library and
movie decoder. Linux DXC does not replace the guest's `dxcompiler_xs.dll`.

```sh
export FROSTY_GAME_ROOT=/absolute/path/to/package
export FROSTY_PREFIX="$PWD/build/codec-test-prefix"
python3 tools/frosty.py media --register --surface-bridge --prefix "$FROSTY_PREFIX"
./Play
```

Register the packaged movie decoder once per new prefix. Normal play reuses the
prefix, saves and translation caches. On a fresh cache, the play profile supplies
the [checked ray dispatch parameters](../profiles/play/rays/dispatch.contract);
the translator takes the actual shaders and view bindings from the game. Existing
prewarmed ray plans take precedence. No compiled game shaders are distributed.

### Local checkouts and separate build directories

Use an empty binary directory to try the build alongside an existing setup:

```sh
export FROSTY_BUILD_ROOT="$PWD/build-release"
cmake --preset runtime -B "$FROSTY_BUILD_ROOT"
cmake --build "$FROSTY_BUILD_ROOT" --target frosty-runtime --parallel 2
```

`./Play` and `tools/frosty.py` use that directory for binaries and default
player state. For first launch there, use `FROSTY_PREFIX="$FROSTY_BUILD_ROOT/codec-test-prefix"`.
You can still override individual providers with `FROSTY_WINE`, `FROSTY_VKD3D`,
`FROSTY_DXGI` or their command-line equivalents.

To use already-patched working copies, add configure arguments such as
`-DFROSTY_WINE_SOURCE=/path/to/wine-frosty` and
`-DFROSTY_VKD3D_SOURCE=/path/to/vkd3d-proton`. Initialize the graphics
submodules recursively in those working copies first. CMake does not fetch,
reset or apply patches to a source override. When configuring over the old
hand-built `build/` layout, it reports and reuses the source paths recorded by
Wine, Meson, Xodus and the codec build. Use a fresh `-B` directory to switch
those existing trees to the locked upstream inputs.

`FROSTY_DISTFILES` can point at a directory containing the named archives from
the lock file; the same hashes are checked. For an offline build, also supply
prepared recursive Git checkouts (`FROSTY_VKD3D_SOURCE`, `FROSTY_DXVK_SOURCE`).

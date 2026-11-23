# Frosty runtime

An x86-64 Linux compatibility runtime prototype for the Xbox Series build of
*The Matrix Awakens*, tested against package `1.20.1831.3754`.
Wine loads the original AMD64 PE and supplies the Win64 ABI. The graphics path
is `d3d12_xs.dll` → vkd3d-proton → Vulkan, with DXVK supplying DXGI.

**Verified on the Radeon RX 6950 XT:** the retail EXE plays the packaged
startup movie and renders the **Enter** screen shown in the reference:
animated green code rain, the Xbox A prompt, and the D-pad Settings prompt.
The [window capture, GPU readback and activation evidence](docs/enter-screen.md)
come from the shipping executable under Wine. On a fresh player, pressing A
here requests the chase. The shipping game calls this widget `BP_CountdownUI`.

**Gameplay bring-up:** the active profile selects hardware ray tracing for
Lumen GI and reflections and keeps Nanite enabled. On RADV the retail main
view built a 265-instance TLAS and executed the shipped hardware direct-light,
radiance-cache, screen-probe and reflection shaders with nonzero GPU work.
[HWRT evidence and reproduction](docs/hwrt.md) includes the fenced dispatch
arguments and triangle-hit checks. The retail game also rendered the **post-FMV
downtown police-car shot** in `CityIntro`. All 893,440 visible window pixels
matched the same GPU present. See the [street capture and comparison](docs/gameplay.md#downtown-street).

**Shader path:** the C++23 host tool decodes Xbox RDNA instructions and stage
metadata, emits HLSL and compiles it to desktop DXIL. The D3D12X guest supplies
that DXIL to vkd3d-proton for translation to Vulkan SPIR-V. Shader hashes
locate cache files; they do not select implementations. See the
[ISA-wide audit and GPU conformance results](docs/rdna2.md) and the
[retail compatibility notes](docs/compatibility.md#instruction-recompiler).

## Try it on this machine

To play, open **The Matrix Awakens** on the desktop or run `./Play`. This
starts the shipping EXE on the current desktop using the existing Wine
prefix, player state and shader cache. Press **Cross (✕)** on a PS5 DualSense,
Xbox **A**, **Enter**, or **Space** at the game's Enter prompt. USB and Bluetooth
use the same [Xbox controller mapping](docs/controllers.md).
Sound is enabled by default. `./Play --mute` opts out. See the
[audio backends and playback checks](docs/audio.md).

The play profile presents on a three-buffer DXGI/Vulkan swapchain in the
original game window. There is no GDI presenter, CPU scanout copy, held
frame, or screenshot observer. The optional `./Play --frame-times frames.tsv`
records timing counters only. The normal launcher averaged **37.16 FPS** over
90 seconds downtown. Every 15-second bin exceeded 30 FPS, but the slowest
rolling second had 20 presents; see the
[measured frame times](docs/play.md#measured-downtown-performance).

From `XBoxGame`, using the existing build:

```bash
export FROSTY_GAME_ROOT="$HOME/Downloads/the-matrix-awakens-an-unreal-engine-5-experience-1.20.1831.3754/436609B6.46723EFB36115_1.20.1831.3754_neutral__9ncxwbgmmv7m8_xs/Mount"
build/host/frosty-shader devices
python3 tools/frosty.py draw
python3 tools/frosty.py draw --test-pixel
python3 tools/frosty.py draw --test-geometry
python3 tools/frosty.py clear --index 409 --group-width 512
python3 tools/frosty.py clear --index 339
python3 tools/frosty.py scene-ids
python3 tools/frosty.py max-pixel
python3 tools/frosty.py probe
```

`draw` reads global shader 0 from the supplied package, translates it, runs a
native x64 guest and checks the GPU readback. No game binaries are included in
the source. `probe` asks Wine to resolve the actual title imports, including
ordinals and delay imports. Its exit code is `3` for missing load-time symbols;
presence of a symbol alone does not establish ABI compatibility.

```bash
# Run the same render check specifically on the Intel ICD on this machine.
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_icd.x86_64.json python3 tools/frosty.py draw

# Register the packaged movie decoder once when setting up a new play prefix.
export FROSTY_PREFIX="$PWD/build/codec-test-prefix"
python3 tools/frosty.py media --register --surface-bridge --prefix "$FROSTY_PREFIX"
./Play

# Select the separate offline executable without replacing the package EXE.
python3 tools/frosty.py probe --exe "$HOME/Downloads/Frosty-XSX-Shipping.exe"
```

The launcher stages app-local DLLs and a linked package view under `build/runs`
for each process. The working directory is `Frosty/Binaries/XSX` and relative
content paths are preserved. `Frosty/Saved` is redirected to the selected
player directory, by default `build/enter-verified-user/Saved` for `play`
and `build/userdata/Saved` for `run`.
`--game` accepts either `Mount` or its parent. Guest command-line arguments go
after `--` on `run` or `play`. The separate developer `run --capture-dir`
can record shader requests; it no longer reads back the scanout.

## Reusing and prewarming shaders

**Normal launches reuse shaders automatically.** The host saves the compiled
DXIL and its binding list under `build/specializations/packets-v1/`. Keys
include the complete original bytes, shader stage, compute wave capabilities,
and actual view or ray-specialization parameters. A different translator,
LLVM, DXC or DXIL validator uses a different namespace. Byte-identical
generated HLSL and compiler arguments can still reuse the `by-source` cache
for the same DXC and validator.

```bash
# After a compiler update: rebuild missing variants from workloads already played.
python3 tools/frosty.py prewarm

# Before playing a streamed city section: prepare related cooked material
# shaders, including integer views seen with the same input/output signature.
python3 tools/frosty.py prewarm --material-families --jobs 4

# Prepare related cooked compute/vertex programs for a wave32/64 + FP16 adapter.
python3 tools/frosty.py prewarm --stage-families --compute-caps 7

# Cover supported graphics stages outside the previously played material families.
python3 tools/frosty.py prewarm --all-graphics

# Precompile the supported compute stages and compatible bound views on the
# RX 6950 XT with the local subgroup-ordinal backend.
python3 tools/frosty.py prewarm --all-compute --compute-caps 15

# Train cull-state siblings and exact bound-view combinations of cooked pairs.
python3 tools/frosty.py prewarm --raster-siblings

# Prepare original XG address maps for large, previously seen textures.
python3 tools/frosty.py tile-prewarm

# Prewarm one capture, without starting Wine. New --capture-dir runs include
# shader-requests.jsonl and the original shader inputs, so this works in a fresh cache.
python3 tools/frosty.py prewarm --from-run build/my-capture

# Older traces contain stage keys but no draw-time view vectors. Seed their
# base stages from the original package (mask 3 = supports both wave32 and wave64).
python3 tools/frosty.py prewarm --from-run build/street-desktop-108 \
  --game "$FROSTY_GAME_ROOT" --wave-caps 3

# Optional first-use precompile of supported VS, PS, GS and CS base stages.
python3 tools/frosty.py prewarm --archive --game "$FROSTY_GAME_ROOT" --wave-caps 3
```

`--archive` also replays learned variants, including ray libraries. It does
not guess resource types, shader-table routes or payload layouts not yet
seen. `run --prepare-shaders` populates this same live cache before launching.
`FROSTY_SHADER_CACHE` or `--cache-dir` selects another host cache; `--jobs`
and `--report FILE` apply to offline prewarming.

The launcher's `frosty/shader-cache` line and a capture's `shader-cache.json`
distinguish disk hits, translator runs and actual DXC compilations.
`frosty/compile-begin` in a guest trace means a **request**, including a hit.
vkd3d-proton separately keeps its per-executable Vulkan-side cache in
`build/shader-cache/` by default. The game still creates pipeline objects;
GPU or driver changes can require new native compilation.

`Play` also learns graphics and compute **pipeline recipes**. These contain
the original shader bytes, root signature, input semantics and render state.
On a subsequent launch workers start after the first present and build
those pipelines and recorded resource-view variants on the current device.
Only an exact recipe can consume a prepared pipeline. Material-family
prewarming broadens the set of candidates, but never substitutes one
material's translated program for another.

The local vkd3d backend also caches DXIL-to-SPIR-V graphics stages. A stage
hit validates its bytecode, root bindings, stage interface and compile
parameters, independently of the complete native pipeline's render state.
Disk packets are checksummed and segregated by the exact backend DLL and
device/compiler configuration. `prewarm --cooked-pairs` prepares representative
recipes from original material-map co-membership in
`pipeline-recipes-v1/training/`. Use `pipeline-inspect --suite <directory>`
to bake those recipes without running the title. The baker uses the shipping
application's driver identity: the driver otherwise selects a different
application cache and shader configuration for the helper. `--raster-siblings`
also prepares matching cull modes and bound-view combinations. Its
`raster-training` directory can be baked the same way.

In two shipping launches through `CityIntro`, the second served **4,487 of
4,489 requests** from disk. Its two compiles were previously unseen requests.
See the [cache measurements](docs/evidence/shader-cache.txt) and
[what `.xsrt` contains](docs/hwrt.md#what-xsrt-stores).

## Wine fork

| Item | Location |
| --- | --- |
| Editable checkout | `../wine-frosty` |
| Runtime branch | `frosty-runtime` |
| `origin` (writable local bare repository) | `../wine-frosty.git` |
| `upstream` | <https://gitlab.winehq.org/wine/wine.git> |
| Upstream base | `97027f879ba9a36dbe594720d4710818536e5433` |
| First runtime commit | `86d9b6ccb3ca` (`mfplat: Implement the Xbox DXGI device manager reset export.`) |

The fork adds the Series-specific `MFResetDXGIDeviceManagerX` entry point to
`dlls/mfplat`. Its arguments are forwarded to the media device manager's
`ResetDevice`. To build the fork out of tree:

```bash
mkdir -p build/wine64
(cd build/wine64 && ../../../wine-frosty/configure --enable-archs=x86_64 --disable-tests && make -j8)
python3 tools/frosty.py draw --wine "$PWD/build/wine64/wine" --test-mf
```

The launcher prefers `build/wine64/wine` when present, otherwise `wine` on
`PATH`. Set `FROSTY_WINE` or pass `--wine` to choose another build.
`play` defaults to `build/codec-test-prefix`. For the other commands the
local fork uses `build/fork-prefix` and system Wine uses `build/wine-prefix`.
Override either with `--prefix` or `FROSTY_PREFIX`. The shader override and
cache paths assume the normal Wine `Z:` mapping to `/`.

## Rebuild the runtime and graphics backend

The [top-level runtime build](docs/build.md) now builds all providers. The
following commands describe the individual development builds.

Host tools need a C++23 compiler and standard library (`std::format`), CMake,
pkg-config, Vulkan headers/loader, zlib, LZ4, LLVM built with the AMDGPU
disassembler, and a Linux [DirectXShaderCompiler distribution](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.8.2505.1).
Extract the x86-64 Linux DXC tarball into `build/deps/dxc` (or set
`-DDXC_ROOT=/path/to/dxc` with `include/dxc/dxcapi.h` and `lib/libdxcompiler.so`).
The PE guest modules
use the x86-64 MinGW-w64 C++ toolchain. vkd3d-proton also needs Meson, Ninja,
`widl`, glslang and its pinned submodules.

```bash
cmake -S . -B build/host -G Ninja -DFROSTY_SUPERBUILD=OFF -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR="$(llvm-config --cmakedir)"
cmake --build build/host
python3 tools/build_audio_codec.py /path/to/unpacked/ffmpeg-8.1.2
cmake -S guest -B build/guest -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/mingw64.cmake"
cmake --build build/guest

vkd="$HOME/OtherShit/vkd3d-proton"
git -C "$vkd" submodule update --init --recursive
meson setup build/vkd3d "$vkd" --cross-file "$vkd/build-win64.txt" --buildtype release
ninja -C build/vkd3d -j8
```

Tested vkd3d-proton revision: `cae8353ed7959c4`. `--vkd3d` / `FROSTY_VKD3D`
accepts a Meson build tree or a directory containing the two x64 D3D12 DLLs.
For DXGI, the launcher prefers `build/dxvk/src/dxgi/dxgi.dll` and falls back
to Steam Proton Experimental. Set `FROSTY_DXGI` or `--dxgi` to another x64 DXVK `dxgi.dll`.
`--dxc` / `FROSTY_DXC` selects an optional x64 DirectXShaderCompiler
`dxcompiler.dll` for Frosty's `IDxcUtils` calls. With `--game`, it defaults to
the package's `dxcompiler_xs.dll`. This guest library is separate from the
host DXC used by the recompiler. The tested host combination is LLVM 22.1.8
(AMDGPU) and DXC release `v1.8.2505.1`.

The local Wine worktree also exports `MFCreateDXGISurfaceBufferX`. The
`draw --test-mf` check passes a genuine D3D12 resource and nonzero subresource
through that entry point. To register the package's own HEVC transform in a dedicated
Wine prefix and verify its activation:

```bash
python3 tools/frosty.py media --game "$FROSTY_GAME_ROOT" \
  --prefix "$PWD/build/codec-test-prefix" --register
```

Use the same prefix for the title. Omitting `--register` checks activation
without changing the prefix. Activation alone does not establish playback.

## Shader development

```bash
pak="$FROSTY_GAME_ROOT/Frosty/Content/Paks/Frosty-XSX.pak"
build/host/frosty-shader inventory "$pak" Global
build/host/frosty-shader inventory "$pak" Frosty
build/host/frosty-shader catalog build/opcode-probes.tsv
build/host/frosty-shader audit "$pak" build/isa-audit --dxil --jobs 12
build/host/frosty-shader inspect "$pak" Global 1
build/host/frosty-shader disasm "$pak" Global 409
build/host/frosty-shader lift "$pak" Global 0 build/inspect-vs
build/deps/dxc/bin/dxc -dumpbin build/inspect-vs/7c562b92e84533dd.vs.dxil
python3 tools/frosty.py draw --shader build/inspect-vs/7c562b92e84533dd.dxbc
build/host/frosty-shader lookup "$pak" 781c3323cca22c4a
```

`lift` writes the original DXBC, generated HLSL and compiled DXIL for one
record. `dump` writes the original DXBC and machine object even when lifting
fails. The low-level `frosty-shader prepare <pak> <dir>` exports loose DXIL
overrides. Use the [launcher prewarmer](#reusing-and-prewarming-shaders)
to fill the cache that normal launches read. `lookup` resolves pipeline log
keys back to archive records; `lift` reports the stage, byte PC and first
unsupported operation. There is no per-shader fallback.

| Source | Responsibility |
| --- | --- |
| `tools/frosty.py` | Wine selection, package view, DLL and shader setup |
| `guest/imports.cpp` | Resolve the PE's imports in the selected Wine loader |
| `guest/d3d12_xs.*` | PE exports, GUID storage and D3D12X ABI boundary |
| `src/archive.cpp` | Pak directory, shader entry table and LZ4 records |
| `src/shader.cpp` | DXBC parts, actual bitcode slot, XDAT/XBCH/FSSM |
| `src/isa.cpp` | DXBC/FSSM stage linkage, export patch tables, LLVM AMDGPU decoder |
| `src/recompile.cpp` | Register, execution-mask, memory, interpolation and export lowering |
| `src/alu_families.inl`, `src/soft64.hpp` | Scalar/vector families, subwords, wave operations and integer binary64 arithmetic |
| `src/audit.cpp`, `src/rdna2_spec.hpp` | Offline whole-archive and AMD catalog probes |
| `src/dxil.cpp` | Compile generated code to desktop DXIL |
| `guest/recompiled_shader.cpp` | Load binary stages and specialize typed views |
| `tools/shader_service.py`, `guest/shader_client.cpp` | Live shader requests and capture of exact specialization inputs |
| `tools/shader_cache.py`, `tools/prewarm_shaders.py` | Shared persistent packets, toolchain identity and offline workload replay |
| `guest/local_user.cpp`, `guest/game_input.cpp`, `guest/save_store.cpp` | Persistent local player, controller readings and disk-backed saves |
| `guest/draw.cpp` | Real D3D12 pipeline, GPU submission and pixel verification |
| `guest/test_clear.cpp`, `guest/test_scene_ids.cpp` | Typed-view, wave control and group-shared memory readbacks |

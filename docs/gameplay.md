# Retail gameplay renderer bring-up

Target: the downtown street as the chase moves from the WakeUp movies to
real-time geometry. The original title runs on Wine, `d3d12_xs`, vkd3d-proton
and RADV on the RX 6950 XT. The external reference image is used for visual
comparison only.

## Active settings

`./Play` uses the [play profile](../profiles/play/UserEngine.ini) with a
GPU-only swapchain, asynchronous loading and queued rendering. The GDI
window comparisons below are retained measurements from earlier builds.

`tools/frosty.py run` now stages [the gameplay profile](../profiles/gameplay/UserEngine.ini)
by default. `--gameplay-profile` and the older `--menu-profile` name select
the same settings. Nanite and the Lumen diffuse indirect/reflection methods
and their `Allow` switches are set to `1`. Traffic, crowds and virtual shadow
maps use the game's settings. The bridge uses the cooked vertex and compute
cluster raster paths rather than the Xbox mesh-stage PSO ABI.

The current phase uses **Lumen hardware ray tracing**. `r.RayTracing=1`,
`r.Lumen.HardwareRayTracing=1` and `r.Lumen.HardwareRayTracing.Inline=1`
select the hardware path. `r.Raytracing.DebugForceRuntimeBLAS=1` makes the
game rebuild BLASes from its own geometry on the active driver. See
[the HWRT bring-up](hwrt.md) for the 265-instance main-view TLAS, four
hardware Lumen passes and the independent hardware hit/vertex readbacks.

The shipping title has also drawn the **WakeUp room in its window** after
the first movie closed. In run 101 its `FTonemapPS` output, the fenced
3840×2160 scanout and the visible area of `UnrealWindowGDK` matched.
[Capture details](evidence/shadow-dispatch.txt) include the original shadow
raygen and its render-thread dispatch.

## Downtown street

On run 108 the **shipping process itself** was held on the police-car shot,
[present 3311](evidence/downtown-held-window.png), in a desktop-visible
Gamescope window. All **893,440** visible pixels of `UnrealWindowGDK` matched
the same fenced 3840×2160 scanout. An independent [desktop window grab](evidence/downtown-held-desktop.jpg)
also matched the scene after compositor scaling. `CityIntro` had no open movie;
the preceding GPU submission included a 101,532-instance TLAS build.
The [reference comparison](evidence/downtown-held-comparison.png) and
[measurements](evidence/downtown-held.json) retain both witnesses.

Run 108 ended 118 seconds
after the hold: Gamescope aborted (exit 134), and its reaper shut down the
shipping process. The verified captures above were saved before that exit.
The play launcher does not pause or copy presented frames to the CPU.

Run 107 reached `ChaseSection-CityIntro` after the fifth WakeUp movie closed.
[Present 3360](evidence/downtown-window.png) shows the police cruiser driving
away down the avenue. The [side-by-side comparison](evidence/downtown-comparison.png)
shows the same camera and vehicle framing as the supplied reference. The
foreground crosswalk stripes are missing and some left-hand facades are less
detailed in this frame.

The external X11 capture of `UnrealWindowGDK` matched **893,440/893,440**
visible pixels of the fenced 3840×2160 scanout, with no open Electra sessions.
At the preceding counter (3359) the engine submitted a **101,341-instance**
TLAS build; Nanite material draws were also recorded during the shot. The
[desktop capture](evidence/downtown-desktop.jpg) shows the live shipping
window alongside the held present. The reference never enters the renderer.

To check the retained artifacts:

```bash
run=build/street-desktop-108
out="$run/held-street"
python3 tools/compare_street.py "$run" 3311 "$HOME/Documents/2026-11-21_11-55.png" "$out"
python3 tools/street_frame_work.py "$run" 3311 "$out/render-work.json"
```

`street-window-3311.timeline` identifies the section and movie sessions;
`street-window-3311.window` identifies the original title process. The
window, scanout and reference hashes are in `measurement.json`. The
selected present has no separately sampled tonemap target; the byte-exact
comparison for this frame is from the scanout to the window.

## Verifying render work

Set `FROSTY_TRACE_WORK=1` and use `--capture-dir` on a graphics launch.
`gpu-work.tsv` samples work **after** its XS command lists have been submitted.
It includes the original shader keys, queue type, presentation counter and
retail recording stack. For indirect commands, `x` is the CPU's maximum
command count; it is not an invented GPU-generated count.

The shipping global cache has 345 sections. `global-index` joins each
section's map hash to its code archive. The type-name tool reads the frozen
type, permutation and `FShader` arrays, uses each instance's resource index,
and matches `FHashedName` to names in the package's shipping symbols. Missing
names remain unresolved. The mapping is diagnostic; shader selection and
recompilation still depend on the original code and actual bound views.

```bash
pak="$FROSTY_GAME_ROOT/Frosty/Content/Paks/Frosty-XSX.pak"
symbols="$FROSTY_GAME_ROOT/Symbols/Frosty-XSX-Shipping-Symbols.bin"
build/host/frosty-shader global-index "$pak" build/gameplay-index
python3 tools/retail_shader_types.py build/gameplay-index "$symbols"
python3 tools/gpu_passes.py "$symbols" build/gameplay-exits-64/gpu-work.tsv \
  --types build/gameplay-index/types.tsv
```

In run 64 the renderer submitted `FPersistentClusterCull_CS` (1,020 groups),
`FMicropolyRasterizeCS` and `FHWRasterizeVS`/`FHWRasterizePS` (indirect),
then `FEmitSceneDepthStencilPS` and `FEmitMaterialDepthPS` (indexed fullscreen
triangles). Run 67 also submitted the virtual-shadow-map cull, raster and HZB
passes. These are verified Nanite workloads in a scene capture, not evidence
that the chase street is visible in the final window.

Run 89 additionally retired nonzero main-view dispatches for the title's
hardware direct lighting, radiance cache, screen-probe GI and reflections.
The material and card draws use the XS rectangle-list topology (`0x11`).
The desktop bridge assembles each three-vertex rectangle into a four-corner
strip **after** the original VS, using its output semantics and interpolation
modes. `build/street-rect-abi/radv.log` checks coverage and UV interpolation
at all 1,024 pixels, including the half missing from a lone triangle:

```bash
python3 tools/frosty.py draw --test-rectlist --game "$FROSTY_GAME_ROOT"
```

The earlier software investigation found that all four GI and both reflection
mesh-SDF records compile, but the title's initial atlas contained zero assets.
`tools/retail_mesh_fields.py` checked the cooked exports against the shipping
static-mesh serializer: 11,755 explicit MDF strip flags, including all six
crosswalk assets; 2,228 static-mesh exports were unresolved. The check uses
the adjacent serialized card data and mesh bounds to locate the flag. An
unresolved export is not counted as stripped.

## GPU checks

The image-atomic fixture checks **one** 64-bit visibility maximum, including
contended texels, distinct high/low words, and old-value returns. On RADV
all 128 returned words and all 74 `R32G32_UINT` texels matched. The explicit
LOD gather fixture checks mip bias/clamps, NPOT extents, mirror/border
addressing, and tap order: 128/128 words on both RADV and ANV. A native
`VK_AMD_texture_gather_bias_lod` comparison on Navi 21 established the
single-mip selection, including half-LOD ties.

```bash
python3 tools/isa_conformance.py --image64 --out build/gameplay-visibility-check
python3 tools/frosty.py isa-check --suite "$PWD/build/gameplay-visibility-check"
python3 tools/isa_conformance.py --gather-lod --out build/gameplay-gather-check
python3 tools/frosty.py isa-check --suite "$PWD/build/gameplay-gather-check"
python3 tools/isa_conformance.py --native-wave --out build/gameplay-wave-native
python3 tools/frosty.py isa-check --suite "$PWD/build/gameplay-wave-native"
```

For compute, the live compiler uses exact-size SM 6.6 wave operations only
when supported by the device; otherwise the guest wave is implemented in
workgroup memory. `build/gameplay-mask-check/radeon.log` verifies 41,728 words
across 19 kernels on RADV, including wave32 and wave64 mask-edge cases.

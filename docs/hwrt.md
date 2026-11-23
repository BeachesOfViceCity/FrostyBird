# Retail Lumen hardware ray tracing

The RX 6950 XT has executed the game's own ray-generation, continuation and
miss instructions through Wine, the XS command-list bridge, local vkd3d-proton
based on `cae8353ed7959c4`, and RADV. The shipping renderer has also submitted
a 265-instance main-view TLAS and executed four **original inline Lumen
compute passes**. A verified post-FMV downtown scanout is a separate milestone.

| Observation | Evidence |
| --- | --- |
| The retail EXE enabled ray tracing, rebuilt from cooked geometry, and submitted a BLAS | `build/gameplay-hwrt-80/engine.log`, `gpu-work.tsv`, `queue-01-type-0.tsv` |
| The retail EXE imported its first translated native state-object collection | `build/gameplay-hwrt-80/launch.log`, collection `d7d2885201fab17d` |
| An inline ray query hit the correct transformed instance, triangle, distance and barycentrics; inclusion mask and ray length produced real misses | `build/hwrt-retail-check/collections.log`, 8/8 independently computed results |
| The shipped `OcclusionMainRG` and `DefaultPayloadMS` gave the correct visibility words | Same log, 8/8 after native collection linking and `DispatchRays` |
| The same raygen dispatched with the shipped `LumenHardwareRayTracingMaterialMS` | `build/hwrt-lumen-check/radv.log`, 8/8 with the six-word diagnostic payload |
| Main view (`RenderViewFamily_RenderThread`), after an earlier empty scene capture | `build/street-pool-88/passes.tsv`: TLAS 265, direct lighting and radiance cache |
| Fenced indirect dispatch dimensions for direct lighting, radiance cache, screen probes and reflections | `build/street-inline-view-89/gpu-indirect.tsv`: `8,448,1`, `64,119,1`, `2,1,1` and `11,1,1`, respectively |
| Native triangle attributes and query-helper readbacks, checked against the cooked indexed triangles | `build/street-walker-check/radv.log`: 72/72 coordinate fields, 80/80 query fields; shipped intersection raygen/miss/hit group 8/8 |
| Offline DXIL for the four Lumen compute families, using the native wave-size specialization | `build/street-hwrt-permutations/dxil.tsv`: all 120 records (4 direct light, 32 radiance cache, 64 screen probes, 20 reflection); compilation coverage, not 120 live dispatches |

The [short evidence transcript](evidence/hwrt-run.txt) contains the readings.
The isolated checks use `Engine/Content/BasicShapes/Cube.uasset` (54 vertices,
48 indexed triangles) and two instances. The CPU oracle intersects the
original indexed triangles and compares the fenced GPU readback. A separate
diagnostic CS checks the detailed hit attributes. It is not supplied to the
game as a replacement shader.

## Reproduce

```bash
utoc="$FROSTY_GAME_ROOT/Frosty/Content/Paks/Frosty-XSX.utoc"
pak="${utoc%.utoc}.pak"
cmake --build build/host
cmake --build build/guest --target d3d12_xs frosty-hwrt-check
python3 tools/hwrt_fixture.py "$utoc" build/hwrt-retail-check
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.x86_64.json \
  python3 tools/frosty.py hwrt-check --suite "$PWD/build/hwrt-retail-check"
python3 tools/hwrt_fixture.py "$utoc" build/hwrt-lumen-check --lumen-miss
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.x86_64.json \
  python3 tools/frosty.py hwrt-check --suite "$PWD/build/hwrt-lumen-check"
python3 tools/hwrt_fixture.py "$utoc" build/street-walker-check --intersection --position-fetch
python3 tools/frosty.py hwrt-check --suite "$PWD/build/street-walker-check"
```

To prepare the supported shaders for a title launch:

```bash
build/host/frosty-shader rt-lift "$pak" Global 1280 build/rt-libraries 6 t1=32,u0=4
build/host/frosty-shader rt-lift "$pak" Global 1282 build/rt-libraries 6
build/host/frosty-shader rt-lift "$pak" Global 1286 build/rt-libraries 6 t1=32,u1=20
build/host/frosty-shader rt-lift "$pak" Global 1237 build/rt-libraries 6
```

`rt-lift` takes an explicit payload size in 32-bit words and structured-view
strides in bytes. It follows the original register moves, memory operations,
stack stores, PC-relative continuation address and scheduler return. The
Xbox `XDXR_TraceRay` call links to the host `TraceRay` intrinsic with the
original AS binding, flags, mask, SBT offsets and ray. Unsupported operands
or linkage are errors. `run` uses `build/rt-libraries` when present;
`--rt-libraries` selects another directory. The bridge compares the entire
incoming serialized collection with the source sidecar before loading DXIL.

## What changed in the title

Run 54 dereferenced a null device interface when the title set residency
priority on an acceleration-structure buffer. The bridge now exposes the
device and list revisions used by the Xbox RHI when the host has DXR.
It translates the title's batched 80-byte AS build records (48-byte DXR
descriptors plus an extension), individual builds, postbuild queries,
residency priority and native state objects. A pure indirect dispatch-rays
signature uses no root signature on desktop D3D12. The profile uses the
game's runtime BLAS build switch rather than deserializing console-driver
BVH bytes as Vulkan acceleration structures.

In run 80 the title recorded three BLAS builds. The sampled queue trace
contains a `build-blas` submission from
`FXboxCommonD3D12CommandContext::BuildAccelerationStructuresInternal`;
the direct queue's diagnostic fence retired every observed submission.
`D3D12.MaxCommandsPerCommandList=1` was used only in this measurement to
flush work before renderer initialization stopped.

The run-80 state-object reflection blocker is resolved. The bridge now
exposes the Xbox selectors and 24-byte function records from each original
collection. The subsequent full-pipeline radiosity dispatch in run 88
requires `FLumenHardwareRayTracingMaterialCHS` and additional ray libraries.
Run 89 retained hardware direct lighting, radiance cache, screen probes and
reflections; it used the package's alternate radiosity tracer and interpolated
translucency volume (`r.LumenScene.Radiosity.HardwareRayTracing=0`,
`r.Lumen.TranslucencyVolume.TraceFromVolume=0`). These are per-run settings.

The compute recompiler recognizes the inlined BVH64 walker from its control
flow and operands, binds the original AS descriptor, and recovers the world
ray, inclusion mask, culling, interval and output registers. Committed
vertices come from `VK_KHR_ray_tracing_position_fetch` on BLASes built with
data access. Ordinary loads in the hit unpacker are resolved from the
committed triangle, instance transform, geometry index and SBT contribution;
the original shader still computes its normal and lighting. Finite iteration
limits count native `Proceed()` steps, not private Xbox BVH nodes; these
units need not coincide.

During `ChaseSection-WakeUp` the title also requested the separate legacy
shadow raygen `FOcclusionRGS` (`Global[3754]`, permutation 22). Its shader
hash was read at the shipping RHI lookup and matched to the original shader
archive. The run's temporary force-all-effects override suppressed that
shader's preparation; the later lookup failed before a hardware dispatch.
[Shadow-raygen evidence](evidence/shadow-raygen-95.txt) records the distinction
from the four working inline Lumen families. An isolated 7×5 ray dispatch
checks the shadow shader's original viewport guard using `--shadow-launch` on
`tools/hwrt_fixture.py`.

With the temporary override removed, the title supplied all 96 cooked shadow
raygen permutations. Run 98 passed the transient MSAA allocation and found
the requested SHA-1 at **index 20 of 99**. Run 99 reached the XS
`DispatchRays(2304,960,1)` entry point; at that point native linking still
stopped on a material collection. Run 100 captured all 742 hit records and
the instance flags in the bound TLAS. See
[the dispatch transcript](evidence/shadow-dispatch.txt).

`--shadow-rays` exercises the complete recompiled raygen on the GPU. In the
7×5 test both output textures matched the expected cube-shadow silhouette:
20 lanes outside the light viewport, 3 occluded and 12 unoccluded inside.
This includes intersections, misses, continuation stack data and UAV writes.

For the title, `--rt-dispatch-libraries` selects a directory prepared with
`tools/prepare_ray_dispatch.py`. The bridge checks the live view formats,
the original SBT uploads and the retired TLAS instance descriptions. It
relocates the reachable identifiers into a native shader table. The
specialized closest-hit path consumes the original collection and a
constant payload word, checked at each `TraceRay`; newly streamed hit
exports can be recompiled by the live host compiler. A fenced status
readback rejects incompatible rays. The isolated suite also deliberately
supplies a wrong SBT route to verify that it reports the actual operands.

```bash
python3 tools/hwrt_fixture.py "$utoc" build/street-shadow-full-check --intersection --position-fetch --shadow-rays
python3 tools/frosty.py hwrt-check --suite "$PWD/build/street-shadow-full-check"
```

## Archive evidence

### What `.xsrt` stores

`.xsrt` is this runtime's name for the **original serialized Xbox ray-tracing
collection**. The bytes start after the cooked `SPTR` tag and stop before
UE's optional record data: the same bytes the title passes to
`DeserializeStateObjectX`. They contain RDNA machine instructions, module
and export metadata, and root signatures. Depending on the collection,
the exported program generates rays or handles misses or hits.

With `--capture-dir`, the guest saves `<collection-key>.xsrt` once per
collection in that capture. On capture completion the launcher verifies
the file's collection key and retains the bytes under their SHA-256 in
`build/specializations/workload-v1/inputs/`. Identical captures share a
hard link on the same filesystem. `prewarm --from-run DIR` also imports
originals from older captures. Prepared library directories keep a copy beside
the desktop `.rt.dxil`, `.checked.dxil` or `.hit.dxil` so the bridge can
compare the original bytes before using the compiled result. For on-demand
recompilation, the shared workload store also retains the exact source by
SHA-256. The [shader prewarmer](../README.md#reusing-and-prewarming-shaders)
reuses translated ray libraries for their recorded export, payload contract
and (for a raygen) resource layout. A source capture alone does not mean the
collection was compiled or dispatched. Native shader identifiers and shader
tables are linked to the current pipeline; they are not on-disk `.xsrt` data.

```bash
build/host/frosty-shader rt-index "$pak" build/hwrt-package/ray-collections.tsv
build/host/frosty-shader rt-list build/gameplay-hwrt-80/d7d2885201fab17d.xsrt
```

All 6,280 `SPTR` records in this package were indexed: 289 raygen, six miss,
and 5,985 hit-group records. Their embedded root-signature-only DXBCs are
not the ray programs. The key covers the bytes the retail EXE actually
passes to `DeserializeStateObjectX`; these 6,280 keys are distinct. The
first capture matches `Global[1280]` byte for byte. In its final module,
`OcclusionMainRG_d7cd43b4.ss_0` is at `0x200` and its post-trace continuation
is at `0x100`. Indexing is independent of translation and execution coverage.

# Playing the shipping game

Open **The Matrix Awakens** on the desktop, run `./Play` from the project
directory, or use `python3 tools/frosty.py play`. It opens the actual
`UnrealWindowGDK` window on the current desktop. Allow the logo to finish
and press **Cross (✕)** on a DualSense, Xbox **A**, or **Enter** at the prompt.
Connect via a USB data cable or pair the controller in Linux Bluetooth settings.
See the [controller mapping and checks](controllers.md).

The launcher finds a unique installed Matrix Awakens package under
`~/Downloads`. `--game` or `FROSTY_GAME_ROOT` overrides discovery. It uses
the local Wine build, vkd3d-proton, DXVK DXGI, the registered codec prefix,
the persistent player, and the shared shader cache. No `/tmp/opencode`
scripts, screenshot tools, automatic controller, or game-state watcher are
part of `Play`. Sound goes through Wine to the selected host render endpoint.
Use `./Play --mute` for the earlier silent profile. The [audio notes](audio.md)
describe the codec paths and independent output checks.

## What runs per frame

- The XS presenter copies the title's scanout directly to a DXGI flip
  back buffer on the GPU. Three buffer slots retain source resources,
  command allocators and lists until their own GPU fences complete.
  Resize and shutdown drain outstanding copies. There is no screen image
  mapping, pixel conversion, GDI blit or CPU presentation fallback.
- The [play profile](../profiles/play/UserEngine.ini) keeps Nanite, hardware
  Lumen and the previous internal screen percentage. Normal RHI command
  submission, deferred contexts, parallel RDG execution, asynchronous
  pipeline compilation, all-eligible asynchronous compute and loading are
  enabled. The diagnostic 512-command forced flush is absent.
- Texture uploads are batched on a GPU transfer queue. Consumer queues wait
  on the upload fence for the textures they actually use. Refreshing mutable
  backing orders a new write behind prior users of that resource. Staging
  contains the requested subresources and is retained until the transfer
  completes. Memory pressure can apply backpressure.
- Large placed textures can be detiled by compute on that queue. Each map
  contains addresses calculated by the package's XG library for the exact
  descriptor and subresource; repeating 16-element patterns are compressed
  losslessly. The input is a checked snapshot of the resident bytes. The
  same XG library supplies the bulk-copy path for a first encounter. Sparse
  mips stay pending until their source pages are available.
- Media copies and ray resources live through submission via bounded queue
  retirement. The ray linker consumes a compact, fenced summary of the
  *specific* TLAS it uses. A previously linked, complete triangle
  closest-hit-only table can be reused while that revision's opacity check
  retires. Opacity, hit-group routing and payload checks
  remain active. Normal play does not dump instance rows or shader tables.
- On the verified RX 6950 XT, play requests wave32 graphics scheduling and
  the backend's native compute subgroup ordinal. The latter is checked on
  the actual device before use. Other adapters keep their portable schedule.
  Its default vkd3d configuration retains PSOs and keeps upload buffers in
  host memory (`no_upload_hvv`). Explicit `RADV_PERFTEST`, `VKD3D_CONFIG`
  and `FROSTY_NATIVE_GROUP_ORDINAL` choices take precedence.
- `tools/storage_pack.py --from-run <run-directory> --gib 6` prepares a
  bounded cache of decoded I/O Store blocks from a `--storage-trace` trial.
  The game still reads the compressed source. The bridge checks its SHA-256,
  the extent, and the prepared block's SHA-256 before serving a hit. A miss
  uses normal DirectStorage decoding with the original completion order.
- The local Wine runtime queues header receipt on the request's worker for
  the game's asynchronous WinHTTP session. The original request, headers,
  error notifications and certificate checks remain in use. A loopback
  fixture checks that a waiting peer cannot block the caller's event pump.
- The native pipeline prewarmer also learns checked ray pipelines. Reuse
  requires identical global and local roots, payload and attribute sizes,
  recursion flags/depth, export names, hit groups and library bytecode.
  The live shader table and its GPU status word still belong to the dispatch.
- Checked ray dispatches reuse table, status and readback buffers only after
  the last recording and its fenced submissions release them. Each dispatch
  writes its own identifiers, local arguments and zero status. A GPU test
  detected an incorrect route on the direct queue, then reused that dirty
  slot on compute and verified all four status words after both replays.

## Optional timing

The [developer showcase check](showcase.md) loads the feature tour without
playing the chase and verifies the original MetaSounds shot and the return
to gameplay.

`./Play --frame-times "$PWD/build/play-times.tsv"` records successful
present timestamps and waits, not pixels. Normally the launcher writes no
timing or capture files. The title and shader cache still use their normal
log and cache locations.

For a warm-cache city sample:

```bash
python3 tools/frame_pacing.py build/play-times.tsv \
  --engine-log build/enter-verified-user/Temp/Logs/Frosty.log --section CityIntro
```

The calculation uses consecutive completed presents inside movie-free
spans. It excludes incomplete intervals at the ends. It is not the
compositor refresh rate. `last_display_stat` is DXGI presentation feedback
when available. Optional worker wall-time counters can overlap each other
and the game thread; they are not GPU-pass durations.

For an isolated, bounded validation at the **end** of a work cycle:

```bash
python3 tools/benchmark_city.py my-city-check --seconds 90
```

Each name creates a new directory under `build/play-validation/`. The
90-second clock starts on entry to the moving city. If the title moves into
`Frogger`, that gameplay remains in the same interval. Movie presents are
excluded; full-window FPS always divides by the requested wall time.
The report includes gaps at either boundary, 15-second bins and the slowest
complete rolling second of movie-free gameplay. To confirm that gameplay
continues through the deadline, the runner waits for a successful present
at or after the cutoff; that present does not enter the frame count.
Only the private validation runner supplies controller input.

`--board` records read-only GPU clocks, utilization, power and temperature
alongside the city interval. `--cpu-samples` also samples title threads;
`tools/city_cpu_costs.py <run-directory>` attributes those samples to the
individual gaps after `stall_costs.py` runs.

The optional CMake guest option `FROSTY_PASS_PROFILE=ON` adds sampled GPU
timestamp pairs around draws, dispatches, indirect work, ray launches, AS
builds and barriers. `--pass-bridge /path/to/d3d12_xs.dll` activates short
sampling bursts in the city; `python3 tools/pass_costs.py <run-directory>`
joins their original shader keys to the cooked shader type index. The
production bridge leaves this instrumentation out.
`tools/stall_costs.py <run-directory>` intersects those gaps with actual
host translator invocations and slow demand-side native pipeline creation.
With `--focus <comma-separated-original-keys>`, GPU timings and their union
cover only those shaders.
`tools/pass_costs.py <run-directory> --window 76 79` restricts attribution
to recording frames in that part of the route. `--stream-samples` on the
benchmark also records the shipping level-purge threshold, pending level
count and process working set. `tools/streaming_costs.py <run-directory>`
joins those samples with the title's streaming and collection events.

## Measured downtown performance

On the RX 6950 XT the following trials used the 3840×2160 swapchain,
Nanite, hardware Lumen and the verified 60% internal screen percentage.
Numbers count successful game presents, not display refreshes.

| Trial | Bridge | Presents / 90 s | FPS | Median | p95 | p99 | Longest gap | Gaps ≥1 s |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `native-flow-async-01` | sampled GPU passes | 1,273 | 14.14 | 50.66 ms | 99.74 ms | 274.82 ms | 7.73 s | 3 including boundary |
| `structured-city-01` | sampled GPU passes | 1,496 | 16.62 | 47.29 ms | 86.48 ms | 169.06 ms | 5.28 s | 3 |
| `release-city-wave32-01` | production, wave32 graphics trial | 1,717 | 19.08 | 44.57 ms | 64.72 ms | 148.62 ms | 2.89 s | 2 |
| `street-trained-04` | sampled GPU passes and CPU threads | 1,221 | 13.57 | 67.75 ms | 108.64 ms | 133.82 ms | 1.42 s | 1 |
| `release-pages-06` | production, wave32 graphics and compute compiler trial | 2,365 | 26.28 | 36.09 ms | 49.68 ms | 74.69 ms | 1.10 s | 1 |
| `release-ordinal-07` | production, subgroup ordinal and culling dispatcher | 2,417 | 26.86 | 35.60 ms | 47.95 ms | 65.19 ms | 531 ms | 0 |
| `release-shadow-08` | production, checked private ray frame | 2,502 | 27.80 | 34.29 ms | 47.51 ms | 69.50 ms | 238 ms | 0 |
| `play-default-10` | normal launcher, production compiler and ray libraries | 2,502 | 27.80 | 34.41 ms | 47.11 ms | 64.53 ms | 191 ms | 0 |
| `city-vertex-13` | production, structured vertex loops, CPU sampling | 2,459 | 27.32 | 35.14 ms | 48.49 ms | 70.09 ms | 252 ms | 0 |
| `release-rows-14` | production, compact TLAS rows, host upload heap and deferred material updates | 2,780 | 30.89 | 31.03 ms | 41.48 ms | 62.60 ms | 205 ms | 0 |
| `city-striped-16` | production, striped descriptor locks, CPU and storage sampling | 3,010 | 33.44 | 28.76 ms | 40.57 ms | 59.68 ms | 264 ms | 0 |
| `release-default-17` | normal launcher, trained storage pack and production bridge | 3,008 | 33.42 | 28.30 ms | 39.99 ms | 61.59 ms | 530 ms | 0 |
| `release-flat-http-19` | normal launcher, flat descriptor pages and asynchronous HTTP headers | 3,092 | 34.36 | 27.79 ms | 38.59 ms | 60.03 ms | 173 ms | 0 |
| `release-pool-20` | production, larger allocation pools and deferred deletion trial | 3,052 | 33.91 | 28.25 ms | 40.10 ms | 63.28 ms | 156 ms | 0 |
| `release-material-depth-25` | normal launcher, cooked early-depth flags and fenced ray packets | 3,344 | 37.16 | 26.11 ms | 34.00 ms | 46.53 ms | 187 ms | 0 |

These measurements do **not** establish sustained 30 FPS. Of 1,716
completed intervals in `release-city-wave32-01`, 54 met 33.33 ms. Host
translation or slow native pipeline creation overlapped 2.85 s of its
2.89 s worst gap. That run had advancing DXGI feedback and no pass-timing
bridge. Earlier release and warm-cache measurements are retained under
`play-release` and `play-warm`.

`release-pages-06` used paged descriptor metadata, native Windows reader/writer
locks and a compute compiler that proves literal barrier sequences. It had
724 intervals at or below budget and six gaps over 100 ms including the end
boundary. There were no downtown DXC compiles; compiling two native variants
of one graphics pipeline overlapped 1.077 s of the 1.098 s worst gap.
`street-trained-04` had no downtown translator or DXC invocations. Its worst
gap instead included 1.350 s of checked ray linking. Neither run paused for a
thread snapshot.

In `release-shadow-08` the live shader/PSO logs recorded **no** downtown
translator invocations, DXC compiles or slow demand-side graphics/compute
pipeline creations. The state-object cache supplied all four native ray
pipelines. There were still five gaps over 100 ms, and only 1,020 of 2,501
completed intervals met the 33.33 ms budget. A separate comparison of the
actual downtown raygen matched all 32,768 RGBA texels with the same
closest-hit and miss shaders; a deliberate route mismatch was detected.

`play-default-10` used no compiler, ray or scheduling overrides. All four
native ray pipelines were reused. The full window had zero host translations,
DXC compiles or slow demand-side PSO creations; four gaps exceeded 100 ms.
1,032 of 2,501 intervals met 33.33 ms. Neither depth-plane copy path was
used in this window. The normal archive preparation visited 32,017 graphics
and 3,233 compute originals: 671 and 14 unsupported sources respectively,
zero unsupported typed variants, and zero other errors.

`release-rows-14` used `VKD3D_CONFIG=retain_psos,no_upload_hvv` and
`r.DeferUniformExpressionCaching=1`. Its six 15-second windows averaged
34.27, 34.00, 30.07, 29.80, 31.60 and 25.60 FPS. The slowest complete
rolling second contained 12 presents. There were eight gaps over 100 ms
and zero measured downtown translations, DXC compiles or slow demand PSOs.
The bridge packed 217,761,559 instance rows for fenced checks in that
window. In a separate GPU test, two revisions of 8,597 rows were checked
on each of the direct and compute queues, including inactive rows,
nonopaque instances and 24-bit hit-group offsets.

`release-default-17` had no scheduling, compiler or library override and
no live CPU or pass sampling. The storage pack served 404,477 verified
blocks (25,865,809,026 bytes) during the city interval with zero rejected
candidates. There were zero downtown compiler invocations or slow native
PSO creations. The last 15 seconds averaged 29.47 FPS; the two roughly
half-second gaps at 71.6 and 73.5 seconds had idle GPU samples.

In `release-flat-http-19` the present containing the HTTP completion at
53.3 seconds took **31.73 ms**, down from 216.00 ms in `release-default-17`.
There were zero downtown translations, DXC compiles or slow demand PSOs.
2,602 of 3,091 intervals met 33.33 ms; the last 15 seconds averaged
29.73 FPS and the slowest rolling second contained 14 presents. In the
pool trial, 114.72 ms of a 155.93 ms gap overlapped a native submission.
An earlier sample at the same shot placed the RHI thread in vkd3d's
allocation-clear `vkQueueSubmit2` while the render thread was allocating
device memory.

In `city-late-clears-23`, Nanite material draws occupied a summed **17.30 ms**
per sampled recording frame at 76–79 seconds (9 samples), versus **3.75 ms**
at 79–90 seconds (36 samples). Of 248 live material pipelines with the
indirect material vertex shader, 241 use a pixel shader that explicitly
requires depth tests before shading. The bridge now carries this flag from
the shipped machine descriptor into desktop DXIL.

Three initialization trials remained below the pacing target. The Mesa
submission-thread trial (`release-driver-submit-21`) averaged 33.10 FPS;
native-zero suballocation (`release-zero-22`) averaged 32.76 FPS. Recording
clears on the caller and publishing their GPU waits on a separate submitter
(`release-sealed-24`) removed the measured long front-end submission calls
but averaged 32.99 FPS, with a 530 ms gap. These are experimental results.

`release-material-depth-25` measured **38.87, 40.20, 37.47, 34.93, 36.47
and 35.00 FPS** in successive 15-second bins. The clock covered the full
movie-free route and DXGI feedback advanced by 3,360. There were zero
downtown translator invocations, DXC compiles or slow demand/replay PSO
creations. Of 3,343 complete intervals, 3,142 met the 33.33 ms budget.
The bridge reused 5,295 ray packets and allocated five. Four gaps exceeded
100 ms: near 37.38, 79.05, 79.20 and 87.40 seconds. Allocation and native
submission each overlapped 88.7 ms of the worst gap; the slowest rolling
second contained 20 presents.

**What the streaming profile controls.** In Ghidra, the package symbols
identify `UWorldPartitionSubsystem::PostInitialize` (RVA `0x519fd40`),
which installs the world-partition threshold. The shipping
`ApplyChaseLoadingSettings` then stores **32** directly into the active
threshold. `UWorld::UpdateLevelStreaming` (`0x515cd30`) checks that value
against the number of levels awaiting purge; an explicit data-layer unload
can reduce a nonzero threshold to one for the next collection. The regular
timed collector has its own interval. In `city-ready-upload-27`, **344 live
samples** saw the active threshold at 32, the partition setting at 64 and the
regular timer at 30 s. The capture also placed the RHI thread inside
`vkResetCommandPool` during the 37 s gap. At 79 s the render thread was
allocating a 646,272-byte acceleration-structure buffer while the RHI
thread was publishing initialization waits.

For these full-window numbers use each run's `frames.tsv` and
`user/Temp/Logs/Frosty.log` with
`tools/frame_pacing.py --section CityIntro,Frogger,CityStreets --seconds 90`.

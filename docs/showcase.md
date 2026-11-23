# Feature showcase: developer shortcut and regression

The feature tour is the packaged `OutroMaster` level sequence. The
developer runner starts a fresh local player, automatically enters Sandbox
after the boot logo, and selects the post-chase showcase. It uses the normal
`./Play` renderer, sound, and installed title; no controller input is needed.

```bash
python3 tools/check_showcase.py my-showcase-check --screenshot
```

Use a new run name. Evidence goes to `build/showcase-validation/<name>/`:
`result.json`, the shipping `user/Temp/Logs/Frosty.log`, `presents.tsv`,
the host audio monitor and, with `--screenshot`, `metasounds.png`. The
runner checks the cooked director's callouts, restoration of the gameplay
profile, and successful presents for five seconds after the outro. The
audio monitor uses a private sink and measures output during MetaSounds.
Add `--wait-for-player` to queue the run behind an open player session.

### How the shortcut works

Only the developer runner builds a temporary two-partition IoStore view.
Partition zero is the installed `.ucas`. The other contains two changed
blocks: the local GameState's Startflow event queues a one-shot request to
enter Sandbox, and the level's short Sandbox intro event selects its
existing `PlayChaseOutro` entry. The new chunk lengths and BLAKE3-160 hashes
are in the private TOC. The title executable, `OutroMaster`, its contents,
the normal launch profile and the real player's save do not change.
`shortcut.json` records the exact asset digests and maps the numbered
sequencer events to their original Blueprint functions.

### Rendering failure

The original Xbox graphics pipeline paired vertex shader `81b25448b7088a4e`
with pixel shader `5daa5a76be3825f2` (`Frosty[19479]`). At byte offset
`0x151c` the latter performs a biased 2D sample using `v26` and `v27`.
The material had used those registers as interpolation destinations
**under a partial execution mask**. The translator discarded the proof
that the other lanes retained valid coordinates and rejected the shader.
UE consequently received `E_NOTIMPL` from pipeline creation. The private
baseline `diagnose-03` reproduced it with the original pipeline, **2.3 s
after the MetaSounds callout**.

The next camera cut exposed a second material, `Frosty[19862]` (pixel key
`5be127c7212ef9f1`, offset `0x1064`). It reuses the rasterizer's front-face
VGPR as a texture coordinate in selected whole quads. The translator now
tracks which acyclic whole-quad mask defined that coordinate, including
saved/restored masks and subsets. An unrelated or widened mask does not
inherit that proof. Quad expansion uses bitwise votes: short-circuit
boolean operators can remove the very lanes whose bits the shader needs.

The pixel lowerer now recognizes a matched P1/P2 pair within one basic
block, without a change of mask or an intervening use of the partial word.
It carries forward only an already-proved complete coordinate. Active
pixels receive the completed interpolant; inactive pixels keep their old
bits. For a masked implicit-LOD sample, gradients and the read-only sample
execute together on participating quads, followed by predicated destination
writes. This prevents DXC from sinking the gradients into a per-pixel
branch. The mip bias remains part of the calculation.

The independent GPU regression is:

```bash
python3 tools/frosty.py draw --game "$FROSTY_GAME_ROOT" --test-pixel-interp
```

On both RADV and Intel it matched **1,024/1,024** pixels. The checker
distinguishes mips 2, 3 and 4, horizontal and vertical gradients, a sample
under a subset of a whole-quad mask, and the inactive lanes' retained values.
Both captured shipping pipelines replayed separately as **2 prepared, 0
unavailable**. No shader hash is a dispatch key for the fix.

### Full shipping retest

[`release-02`](../build/showcase-validation/release-02/result.json) fired all
ten authored feature callouts. The game rendered the
[MetaSounds card and city scene](../build/showcase-validation/release-02/metasounds.png)
past the original failure, played the MetaHuman callout, and logged
`SandboxIntroFinished`. The private stereo sink measured **8.95 seconds of
non-silent output during the MetaSounds card**; both sink inputs belonged
to the shipping process. There were **3,447** successful presents during
the outro and **147** more after it finished. The run recorded no failed
pipeline or shader requests.

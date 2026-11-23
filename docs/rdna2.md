# RDNA 2 recompiler coverage

The ISA catalog, the title's shader archive, and GPU conformance are three
different measurements. A name in the catalog is **not** evidence that
arbitrary operands or shaders using it execute correctly.

## Run the complete inventory

```bash
pak="$FROSTY_GAME_ROOT/Frosty/Content/Paks/Frosty-XSX.pak"
build/host/frosty-shader catalog build/opcode-probes.tsv
build/host/frosty-shader audit "$pak" build/isa-audit --dxil --jobs 12
```

- `catalog` tries a base encoding for **each of the 1,153 documented
  mnemonics**. The AMD XML lists 4,367 encoding alternatives. LLVM
  independently decodes 1,135 base specimens to the expected mnemonic and
  length. For the other 18 horizontal/packed image operations, the decoder
  reports the vendor fields, register bases and instruction length; their
  texel formats and return widths still need execution support.
- The last column tries the **production lowerer** on each specimen in an
  isolated compute fixture. It records missing context and failures as well
  as passes. The catalog does not execute arbitrary system or memory
  instructions on a GPU.
- `audit` walks reachable instructions in **both** shader libraries
  independently of the stage linker. A failed translation does not conceal
  later decoded opcodes in that shader. Direct edges, terminators and the
  vertex fetch convention determine reachability; inline data is not counted
  as shader code.
- `records.tsv` gives one result per archive record, with the library/index,
  cache key, decoder status, lowering status, default-numeric DXIL result,
  failure PC and original instruction where available. `opcodes.tsv` includes
  all vendor mnemonics, even unobserved ones. `forms.tsv` retains LLVM suffixes
  and modifiers; `issues.tsv` ranks first failures.
- `--require-all` enables the DXIL pass and returns status 3 if any archive
  record is not covered. A normal inventory succeeds when the **scan** is
  complete even when it reports gaps. Default DXIL is not evidence of every
  numeric binding combination or of pipeline execution.

The retail package has 43,407 records: 37,127 have a recognized FSSM machine
object; the other 6,280 are reported separately. The machine records include
192 mesh-stage programs whose ISA can be inventoried without a desktop mesh
stage linker. Their combined vocabulary is 339 RDNA 2 mnemonics plus three
Xbox-retained legacy operations (`v_mac_f32`, `v_mad_f32`, `v_madak_f32`).

### Measured package results

The downtown build's lowering audit (`build/street-city-shader-audit-107`)
decoded 37,127 machine programs and lowered **36,246**: 642 more than
`build/street-shader-audit-106`, with no previously lowered record lost.
This audit did not compile DXIL archive-wide. On both RADV and Intel,
the pixel fixtures independently matched 1,024/1,024 pixels for masked
elections and relative VGPR access, and 1,024/1,024 for cube-array face,
layer and mip sampling under nested EXEC masks.

The earlier full DXIL pass measured:

| Library | Archive records | FSSM programs decoded | Default DXIL compiled |
| --- | ---: | ---: | ---: |
| Global | 4,576 | 4,271 | 3,029 |
| Frosty | 38,831 | 32,856 | 29,830 |
| Total | 43,407 | 37,127 | **32,859** |

In `build/rdna2-final`, **4,268** machine programs still failed
lowering. All 32,859 that lowered compiled to DXIL. The strict audit returned
3 as intended. Before this pass, the same whole-archive lowering measurement
was 4,526 records (`build/rdna2-audit-before`; no DXC pass on that baseline).
In the isolated catalog fixture, **800** instruction specimens lowered.
These are different denominators.

The later Enter-screen bring-up added two general cases: a chain of vector
operations can retain a uniform value within an EXEC arm, and scalar ALU can
extract width, height and base-mip bits from a bound image descriptor. The
second case uses the original resource size and the actual SRV's mip, including
when a descriptor is copied into a shader-visible heap. `Global[2649]` and
`Global[3606]`, the lighting pipeline reached from the Enter screen, compile
through these paths.

`build/enter-isa-audit` lowered **33,308** machine records (3,095 Global,
30,213 Frosty), 449 additional records with no formerly successful lowering
lost. A separate DXC pass over those 449 newly accepted records compiled
**449/449** default specializations (`build/enter-isa-audit/new-dxil.tsv`).
The 32,859 default-DXIL count in the table is from the earlier full DXC
pass. The wave-size-dependent
`readfirstlane` diagnostic remains for 636 records in the newer inventory.
The rebuilt 14-kernel conformance suite in `build/enter-isa-check` again
matched all 39,680 reference words on each of RADV and ANV.

The [per-opcode matrix](rdna2-coverage.tsv) joins the specimen result, retail
record counts, one archive witness and the independently checked GPU cases
containing that mnemonic. Presence in a passing case is not an exhaustive
test of all its operands. To regenerate the matrix from a new run:

```bash
python3 tools/isa_matrix.py --audit build/rdna2-final \
  --catalog build/rdna2-catalog.tsv --suite build/isa-conformance \
  --gpu-log build/isa-conformance/radv-release.log \
  --gpu-log build/isa-conformance/anv-release.log --out docs/rdna2-coverage.tsv
```

## GPU conformance

```bash
python3 tools/isa_conformance.py
python3 tools/frosty.py isa-check --suite "$PWD/build/isa-conformance"
```

The generator uses Python 3.13+ (`math.fma`), `llvm-mc` and `llvm-objcopy`
to produce raw gfx1030 code.
`test-kernel` supplies an isolated compute ABI; retail shaders get theirs
from their own FSSM and DXBC. The PE runner dispatches the lifted DXIL
through vkd3d-proton, fences, and compares readback with independent CPU
reference words.

Cases include all **190** vector compare/CMPX mnemonics, floating-point
classification and NaNs, carry/borrow, packed selectors, SDWA byte preservation,
DPP, cross-row transfer, and actual inter-wave LDS communication in wave32
and wave64 mode. The binary64 cases cover add, multiply, fused multiply/add,
reciprocal, square root and conversion. The Intel device does not expose
native shader FP64. The fused test includes a result that would be zero with
a separately rounded multiply and add.

**Result:** 14 kernels and **39,680/39,680** output words matched on each of
RADV and ANV. This includes an LDS allocation reused across real guest
barriers, concurrent halfword stores, a 64-bit exchange with both original
words read back, subword buffer writes and M0-relative VGPR reads.

For retail-kernel checks, `max-pixel` exercises a six-workgroup atomic
reduction and `clear --index 339` checks a counted Xbox `ExecuteIndirect`,
including the tail that an unintended second indirect command would modify.

## Implementation boundaries

`src/alu_families.inl` lowers scalar/vector families, including sub-dword
selection, packed lanes, wide values and atomics. `src/soft64.hpp` implements
binary64 operations using integers. `src/recompile.cpp` handles execution
masks, resource provenance, texture access, interpolation and exports. For
multiwave compute it preserves per-wave registers and PCs and parks waves at
guest workgroup barriers.

Remaining work includes unrestricted flat/global GPU virtual addresses,
scratch/GDS and ordered-count facilities, BVH node operations and ray-stage
linkage, additional graphics wave/derivative cases and non-singleton
descriptor layouts. Unsupported cases remain explicit diagnostics.
The largest remaining retail failure group in this measurement was
wave-size-dependent `readfirstlane` in graphics stages (1,156 records).
Full device FP-mode, exception and privileged-register behavior is not
established by the round-to-nearest binary64 conformance cases.

The index comes from AMD's [machine-readable ISA](https://gpuopen.com/machine-readable-isa/),
RDNA 2 XML release `2026-02-20`, schema `1.1.1`. Regenerate it with
`python3 tools/rdna2_catalog.py /path/to/amd-isa.zip`; the generator verifies
the XML SHA-256. See the [AMD catalog license](amd-isa-license.txt).

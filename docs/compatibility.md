# Compatibility findings: Frosty 1.20.1831.3754

## Executable

Ghidra's active program `/Frosty-XSX-Shipping.exe` is the package executable,
not the alternate file in Downloads. Both are AMD64 PE32+ GUI executables with
preferred image base `0x140000000`, entry RVA `0x05e70fa8`, a TLS directory
and x64 unwind data. Wine handles these and the Windows calling convention
while the CPU executes the guest instructions natively.

| File | SHA-256 |
| --- | --- |
| `Mount/Frosty/Binaries/XSX/Frosty-XSX-Shipping.exe` | `3eff72f4ee4162d6ae47196f8eaefc7d9399f310a16c30ffb21439bb3477ede4` |
| Separate offline EXE | `ae39eb1e92fa806df189c768c1478a53dfb54a2cc9769a9f2301ca7921e3eb04` |

Both have 537 eager imports from 65 DLLs, plus 15 delay imports from
`GameChat2.dll`. With stock Wine 11.0, DXVK DXGI, the local vkd3d-proton
build and the XS graphics shim, before adding a DXC provider, `probe` found:

| Unresolved provider | Imports |
| --- | ---: |
| `dxcompiler_xs.dll` | 1 |
| `xmem.dll` | 9 |
| `xg_xs.dll` | 1 |
| `XCRDAPI.dll` | 1 |
| `AcpHal.dll` | 7 |
| `MFPlat.dll!MFResetDXGIDeviceManagerX` | 1 |

A launch of the package EXE on stock Wine stops at `STATUS_DLL_NOT_FOUND`
(`0xc0000135`) before the entry point. The offline EXE has the same import
blockers. `probe` measures symbol resolution, not implemented behavior; DLLs
loaded later via `LoadLibrary` are outside its scope.

**Current runtime:** all 537 eager and 15 delay imports resolve. The launcher
uses the package's `xg_xs.dll` and `dxcompiler_xs.dll`, the guest audio,
XMem and DirectStorage implementations, and the Wine fork. The EXE
mounts its IoStore containers, loads `/Game/Map/P_Construct_WP`, and enters
`EFrostyGameStateType::Startflow`. The local GDK adapter now adds a persistent
player and associates the local [GameInput gamepad seats](controllers.md) with
that player. The adapter stores
saves under the selected `--userdata`; a new profile has no `FrostySaveData`
container. No Xbox Live token is issued. In a fresh-player run the title's
HTTPS request received a 236-byte HTTP 200 reply from Epic's CDN and
`AFrostyGameMode::OnStartflowDetermined` received `Countdown` (2).

## ABI evidence

* **D3D12X:** The six named imports are `D3D12CreateDevice`,
  `D3D12SerializeRootSignature`, `D3D12SerializeVersionedRootSignature`,
  `IID_ID3D12Device` (data), `D3D12XboxSetProcessDebugFlags` and
  `D3D12XboxCreateDevice`. The first three are PE forwarders to vkd3d-proton;
  the GUID is exported as storage. Debug flags `0` are accepted.
* At VA `0x145752bc5` the retail caller passes a null adapter, a parameter
  block beginning with `0x20004`, the `ID3D12Device` IID and an output pointer.
  It then queries the returned root device for `IDXGIDevice1`, obtains its
  adapter and queries for `IDXGIFactory2`. In one branch it also uses an
  Xbox-specific device vtable slot. A desktop `ID3D12Device*` is not a
  substitute for this ABI. `guest/xs_device.cpp` implements the Xbox root,
  queues, command lists, descriptor heaps and PSO wrappers over vkd3d-proton.
  Presentation copies the game's Vulkan-rendered scanout GPU-to-GPU into
  the DXGI flip swapchain of its `UnrealWindowGDK` window. Each back buffer
  has its own fence-tracked command allocator.
* **Media Foundation:** At `0x141983dc8`, RCX is the manager from
  `MFCreateDXGIDeviceManager`, RDX is the device, and R8D is its reset
  token. The caller checks the returned HRESULT. The Wine fork implements
  this export using the existing `IMFDXGIDeviceManager::ResetDevice`. The
  `--test-mf` draw check rejects a wrong token and retrieves the original
  D3D12 device from `GetVideoService` after resetting with the right one.
* **DXC:** Both `DxcCreateInstance` calls (`0x145767c54`, `0x145767cae`)
  request CLSID `6245d6af-66e0-48fd-80b4-4d271796748c`, IID
  `4605c4cb-2019-492a-ada4-65f20bb7d67f` (desktop `IDxcUtils`).
  Supplying an x64 desktop DXC under the `dxcompiler_xs.dll` import name
  works for container introspection. This does not restore stripped LLVM
  bitcode or add Xbox code generation to desktop DXC.
* **Memory:** The title imports the default allocators, working-set queries
  and physical page allocation/mapping from `xmem.dll` and exports
  `__XMemAllocationHooks__`. Ordinary heap allocation alone does not supply
  title allocation callbacks, GPU visibility or page-mapping semantics.
  `guest/xmem.cpp` implements the title hooks and aliased page mappings.

## What the shaders actually contain

The two `SF_XSX_SM6.ushaderbytecode` libraries are unencrypted, uncompressed
Pak v11 **entries** in `Frosty-XSX.pak`. Individual **shader records** inside
them are LZ4 compressed. Their location is looked up by name in the full
directory index; no title-specific file offsets are compiled into the reader.

| Library | Records | Indexed DXIL + XDAT/XBCH + NOBC | Other records |
| --- | ---: | ---: | ---: |
| Global | 4,576 | 4,271 | 305 |
| Frosty | 38,831 | 32,856 | 5,975 |

For the 37,127 records in the third column, the DXIL program header points
to four bytes reading `NOBC`, not LLVM bitcode. The machine object is a
zlib-compressed `FSSM` payload in `XBCH` v3, reached through the `XDAT` part
directory. Checking the DXBC/DXIL magic alone would send these shaders to
the wrong compiler. The other records include ray-stage data with embedded
root signatures and are not handled by the VS/GS/PS/CS recompiler.

## Instruction recompiler

`src/isa.cpp` validates the FSSM preamble against the DXBC `HASH`, then uses
LLVM's AMDGPU decoder on the actual program. C++ lowering in
`src/recompile.cpp` is selected by instruction and operands, **never** by
shader name, body length, or hash. It tracks SGPRs, VGPRs, lane predicates,
descriptor provenance, and control-flow joins. Unsupported encodings, memory
operations and interfaces produce a diagnostic, not an executable stub.

* **Inputs and resources:** FSSM maps descriptor-table SGPRs and byte offsets
  to the bindings in `PSV0`. Vertex fetch and parameter export records identify
  signature registers. Pixel inputs use both the hardware interpolant map and
  the PSV interpolation qualifier. Compute group dimensions and system
  registers come from the compute descriptor. Geometry shaders with the
  supported triangle ABI map LDS vertex-ring reads and writes to per-vertex
  signature components; `GS_OP_EMIT` appends the vertex actually assembled.
* **Control and memory:** supported operations include scalar/vector math,
  per-lane `EXEC`/`VCC`, nested forward regions, structured loop back edges,
  CBV loads, structured and raw buffer transfers, typed stores and texture
  sampling. The compute dispatcher implements a guest wave with workgroup
  votes and LDS operations. Multiwave groups can time-slice those waves and
  synchronize at `s_barrier`, even across narrower host subgroups.
  Vertex, pixel, and compute stages can specialize independent typed
  bindings. The root signature supplies the table and offset; the bound
  SRV/UAV supplies the numeric type at the actual draw or dispatch. Acyclic
  graphics control flow can be emitted in topological order, including jumps
  across adjacent structured regions. PC-relative global loads in supported
  shaders read the constant data embedded in the original machine record.
* **Outputs:** pixel shaders contain selectable hardware export sequences.
  The linker validates the live sequence and selects the package's full-width
  export instructions before decoding them. This retains integer bits and FP32
  precision; the host graphics pipeline performs the actual RTV conversion.
  Color-only pixel stages also retain the original `DB_SHADER_CONTROL`
  depth-before-shader bit. Of 12,351 cooked pixel records, 3,512 request
  early tests. Ordinary early-Z eligibility alone does not suppress the
  shader's UAV writes for fragments that fail depth.
* **Backend:** during a launch the guest submits the original container and
  numeric types to the local host compiler. DXC produces desktop SM6 DXIL;
  vkd3d-proton maps the root signature, links stages and emits SPIR-V. The
  on-disk game and original shader containers are not modified.

An earlier offline `prepare` run on `1.20.1831.3754` compiled **1,938 of 43,407 archive
records** (including DXC compilation of each required numeric variant):

| Archive | Vertex | Geometry | Pixel | Compute | Total |
| --- | ---: | ---: | ---: | ---: | ---: |
| Global | 64 | 1 | 445 | 235 | 745 |
| Frosty | 33 | 0 | 1,159 | 1 | 1,193 |

These records contain 1,928 distinct DXBC cache keys. This scan predates
the live compiler; it is not a current coverage measurement. The tests
below check individual programs on the GPU.
See the [ISA-wide audit](rdna2.md) for the new full-corpus procedure.

### GPU checks

| Retail program | What was checked | Result |
| --- | --- | --- |
| `Global[0]` / CopyVS | Nonzero start vertex, interpolated RGBA8 gradient | 1,024/1,024 pixels on RADV and ANV |
| `Global[2536]` / PixelMain | Interpolated gradient on an RGBA32F target; blue is `0.1234567f`, not FP16-representable | 1,024/1,024 pixels on RADV and ANV |
| `Global[2420]` / WriteToSliceMainGS | Two instanced quads; separate color and gradient readbacks from two array slices | 2,048/2,048 pixels on RADV and ANV |
| `Global[402]` / ClearCS | Typed uint4 buffer, 64 threads, untouched prefix and tail | 336/336 channels on RADV and ANV |
| `Global[409]` / ClearCS | Different retail body, **512 threads**, two groups and a partial tail | 2,128/2,128 channels on RADV and ANV |
| `Global[1320]` / GPUSceneSetInstancePrimitiveIdCS | Empty, two-span, five-span and 64-span batches; extra out-of-range group, sparse destinations and preserved payload bits | 640/640 words on RADV and ANV |
| `Global[339]` / ClearIndirectArgInstanceCountCS | Scalar `R32_UINT` UAV; clear only instance counts inside five-word indirect draw records | 420/420 words on RADV and ANV |
| `Global[2633]` / MainCS | Six 8×8 groups, brighter out-of-bounds texels, LDS max and a compare/exchange lock protecting the winning coordinates | RADV and ANV: `(17,10)`, luminance `0x3f970a3d`, lock released, both sentinels preserved |
| `Frosty[18398]` / Nanite material pixel shader | Depth-equal rasterization with 100%, 12.5% and 0% of pixels passing. The control clears only the original depth-before-shader bit; both programs write an extra per-pixel UAV witness. | RX 6950 XT: all 65,536 UAV checks and 524,288 render-target words matched expectations in each case |

The standalone draw tests use an independently compiled companion stage;
they verify GPU execution and readback, not game state. The retail boot test
instead uses the game's own shader pairs and the real window.

### Retail boot checkpoint

The Radeon rendered the title's animated loading dots in the original window
(`build/menu-live-27/frame-000128.png`). The independent vertex SRVs,
pixel UAVs, compute typed loads and `Global[16]` control-flow join that
blocked earlier runs now translate. D3D12X list slots 53 and 68 forward
the integer UAV clear and the dynamic depth bounds, respectively.

In `build/menu-live-28/launch.log` the graphics run reached
`UFrostyUIComponent::SetConnectionScreenActive` with `DL=1`. The packaged
`BP_FrostyUI.AddRelevantStartUpScreen` handles `EstablishingConnection` (1),
`Countdown` (2) and `MainMenu` (3) separately. On a new profile, `Enter`
after the countdown panel's fade-in requests the chase. Neither `Startflow`
nor the `NativeConstruct` address shared by the two widget classes is proof
of a visible main menu.

`build/menu-bulk-40` reached present **5,856** with no shader-creation error.
Both the GPU frames and timed window captures after loading were black.
Electra opened the actual 3840×2160 video stream, but reported
`MFTEnumEx returned zero activation objects`. `build/menu-bulk-41` recorded
the connection panel and the face-left skip input; no main-menu activation
was recorded in that run.

The package contains its own `mfhevcdec.dll`. Its only missing eager import
in the prior Wine build was `MFCreateDXGISurfaceBufferX`. The new Wine
worktree export wraps the D3D12 resource and subresource in an MF buffer.
With the transform registered in the separate codec-test prefix, the
package's class factory constructed an `IMFTransform`, advertised HEVC
input, and activated with Electra's synchronous enumeration flags. The
standalone evidence is `build/media-probes/launcher.log`.

With that decoder registered, `build/menu-codec-43` passed the previous
enumeration failure and reached frame **9,500**. Electra's next failure was
`Failed to set video decoder output type` (`MF_E_INVALIDTYPE`, `0xc00d36bd`)
when selecting NV12.
The later GPU and window captures were black; the main-menu breakpoint did
not fire.
The separate `build/menu-codec-format-44/launch.log` trace shows a 2,532-byte
`MF_MT_USER_DATA` and `MF_MT_FRAME_SIZE` of 1920×1088 during negotiation.
With those same packaged HEVC configuration bytes and no device-manager
message, the standalone transform accepted NV12 at both that size and
3840×2160. That result is not proof of video playback in the title.

The [Enter-screen verification](enter-screen.md) supersedes that black-screen
checkpoint. In `build/menu-surface-45` the shipping Electra player decoded the
original movie, reached its end at 21.022 seconds, and allowed controller A
to request the chase. In `build/menu-rain-sync-50` the same title rendered the
animated background and both button prompts on the first-run Enter screen.

## Native RDNA 2 path

The RX 6950 XT reports `1002:73a5` (Navi 21). Sharing gfx10.3 instruction
encodings does not establish agreement on user SGPRs, resource descriptors,
wave size, relocations, stage linkage or render state. Vulkan shader modules
for this backend consume SPIR-V. Vulkan pipeline binaries and binary shader
objects are driver-defined formats, not Xbox machine-shader import APIs.

Direct reuse would require a driver/backend interface that accepts the Xbox
code and translates its stage and resource ABI. Until that exists, both AMD
and non-AMD devices use the portable recompiler. Additional descriptor and
stage ABIs, larger LDS allocations and ray shaders still need implementation.

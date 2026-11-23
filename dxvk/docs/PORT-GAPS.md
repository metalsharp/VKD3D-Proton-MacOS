# What the DXVK 3.1 port still needs

*2026-10-28. Compares Gcenx's DXVK-macOS 1.10.3 (98 commits on upstream v1.10.3) with this fork at `8d348236`
(upstream v3.1.1 plus `40e01640`), against the MoltenVK in MetalSharp Wine 4.0. Capabilities come from
`docs/baselines/baseline-M4-macOS27-metalsharp-wine-4.0.json`; see `docs/gap-policy.md`.*

The fork's goal is one DXVK for MetalSharp's Arm64 FEX build: the D3D8/9/10/11 lane (`GRAPHICS_BACKEND=dxvk`, today
DXVK-macOS 1.10.3) and the DXGI and D3D11On12 half of the VKD3D-Proton lane (today plain upstream DXVK 2.7.1, built for
x86_64 only, so it runs under FEX).

## What the MoltenVK probe changed

MetalSharp's MoltenVK passes every feature and extension DXVK 3.1.1 requires. It also has geometry shaders (as Metal
mesh pipelines), stream 0 transform feedback, `VK_EXT_depth_clip_enable`, cull distances (as clip distances),
`robustBufferAccess2` and `nullDescriptor`. Several items the 2026-09-21 gap policy treated as permanent gaps are
therefore checks, not workarounds, and the five requirements this fork relaxed in `dxvk_device_info.cpp`
(`geometryShader`, `shaderCullDistance`, `depthClipEnable`, `robustBufferAccess2`, `nullDescriptor`) can return to
upstream's strictness. A MoltenVK without them would then be rejected at device creation instead of rendering wrongly.

Still missing in MoltenVK: graphics pipeline libraries, pipeline statistics queries, custom border colors, descriptor
buffers, present timing, multiple transform feedback streams, and correct geometry shader limits (all reported as 0).

## D3D11 and the VKD3D-Proton lane: status

| # | Item | Status |
|---|---|---|
| 1 | Pipeline-statistics query crash (`dxvk_gpu_query.cpp`, `d3d11_query.cpp`) | **Done** (`faec9f8f`): such queries return zeros. Stream-output queries do the same for streams the device lacks |
| 2 | D3D11On12 device check (`dxvk_adapter.cpp` `importDevice`) | **Done** (`93d57c74`): logs the first requirement the imported device misses. VKD3D-Proton-MacOS now enables the two it missed, `inlineUniformBlock` and `VK_KHR_load_store_op_none` |
| 3 | The VKD3D lane's DXGI fixes (VKD3D-Proton-MacOS `patches/dxvk/0001`) | **Done** (`93d57c74`): flip-model buffer counts and the D3D11On12 error message; the other two changes are upstream |
| 4 | ARM64X build | **Done** (`53b4318e`, `build-arm64x.txt`): dxgi, d3d11, d3d10core, d3d9 and d3d8 build as ARM64X. CI still builds only x86_64 and i386 |
| 5 | Geometry shader limits | **Done** in MoltenVK (VKD3D-Proton-MacOS): Vulkan's minimums (256 output vertices, 1024 total components, 32 invocations) instead of 0 |
| 6 | Stream output | **Open.** MoltenVK crashed creating the pipeline (`newMTLGeometryRenderPipelineDescriptor` read the null `pMultisampleState` of a rasterizer-discard pipeline); that is fixed in VKD3D-Proton-MacOS, but the stream output buffer stays empty. DXVK 2.7.1 on the same MoltenVK writes nothing either |
| 7 | Restore upstream's requirements | **Done** (`d8ab94b8`) |
| 8 | Depth clip guard | Not needed while item 7 holds: a device without `VK_EXT_depth_clip_enable` is rejected |
| 9 | Feature level reporting | Not yet checked in games |
| 10 | A smoke test that draws | **Done**: `tools/smoke/d3d11_draw.c` |

### `tools/smoke/d3d11_draw.c` results

x86_64 test program under MetalSharp Wine 4.0 with `GRAPHICS_BACKEND=vkd3d` (the VKD3D-Proton lane, where DXVK
provides dxgi, d3d11 and d3d10core), Apple M4, 2026-10-28. "Shipped" is that lane as MetalSharp Wine 4.0 ships it
(DXVK 2.7.1 for x86_64, MoltenVK at d1070b55); "this branch" is DXVK at this branch as ARM64X with the
VKD3D-Proton-MacOS MoltenVK and VKD3D-Proton changes.

| Test | Shipped | This branch |
|---|---|---|
| `draw` | pass | pass |
| `depth_clip` | pass | pass |
| `pipeline_stats` | crash (null query pool) | pass (zeros) |
| `stream_output` | fail (buffer empty) | fail (buffer empty) |
| `geometry_shader` | fail (nothing drawn) | pass |
| `d3d11on12` | pass | pass, device meets every DXVK requirement |

VKD3D-Proton's `test_draw_instanced`, `test_clear_render_target_view`, `test_fragment_coords`,
`test_shader_instructions` and `test_geometry_shader*` give the same results on both.

## Needed later

| Item | Notes |
|---|---|
| Async pipeline compiling (P4) | No graphics pipeline libraries, no state cache (removed upstream in `57d5a1e4`) and no `VkPipelineCache`, so every new pipeline compiles while the game waits. 1.10.3's `dxvk_pipecompiler` maps onto 3.1.1's `DxvkPipelineWorkers` at `dxvk_graphics.cpp:1086-1115` |
| D3D9 render-target copies | 1.10.3 `7865799f` and `91fdf892` |
| D3D9 shadow samplers | 3.1.1 de-aliases 3 image variants per sampler, 1.10.3 had 5. Check shadow samplers |
| App profiles | LEGO and KSP2 from 1.10.3 |
| Stream-output `S_OK` fallback | 1.10.3 `ce15afaa`. Only for a MoltenVK without transform feedback |
| Docs | README's `../patches` path (CI uses `../../patches`), its `dxvk.conf` claim, and the state cache mentioned in `PHASE2-REPORT.md` |

## Direct3D 7, 6, 5 and 3 (d7vk `ddraw.dll`)

*2026-11-21. d7vk `devel` at `9fb6a2d50`, merged with upstream master `0f5c295c`. Apple M6, MoltenVK 0.2.2211.*

`tools/smoke/ddraw_draw.c` draws into offscreen DirectDraw 3D surfaces and reads them back: a D3D7 clear, D3D7
pre-transformed draws, an R5G6B5 render target, a locked 16-bit Z buffer, source color keying, and a D3D6, D3D5
and D3D3 (execute buffer) draw. Every check passes on Wine's own ddraw and on d7vk's, which loads, uses MoltenVK
and logs no errors:

| Wine | Program | `ddraw.dll` | Result |
|---|---|---|---|
| MetalSharp Wine (arm64, FEX) | i386 | i386 | 8/8 |
| MetalSharp Wine (arm64, FEX) | x86_64 | ARM64X | 8/8 |
| MetalSharp Wine x86_64 | i386 | i386 | 8/8 |
| MetalSharp Wine x86_64 | x86_64 | x86_64 | 8/8 |

`tools/smoke/d3d9_draw.c` passes 8/8 on the same three D3D9 builds, and `d3d11_draw.c` gives the results above
(`stream_output` still fails).

No MoltenVK change was needed. Still to check in games: rendering to A4R4G4B4 (a swizzled format in MoltenVK),
D24 depth read back from its D32 fallback, multisampled surface locks, and wined3d (Wine's ddraw) and DXVK
presenting to the same window.

| Item | Status |
|---|---|
| ARM64X | **Done**: d7vk's SSE2 vertex processing took ARM64EC for x86 (`ddraw_include.h`); it now uses the scalar path there |
| d3d9.dll beside ddraw.dll | **Done**: D3D9 is one static library, linked by both; each DLL has its own logger |
| MetalSharp Wine routing | **Done on branches, not released**: with `GRAPHICS_BACKEND=dxvk` alone (no copied DLLs, no overrides), both Wines' loaders give i386 and x86_64 programs the lane's `ddraw.dll`, and `ddraw_.dll` is Wine's own. MetalSharp Wine (arm64): `source/wine` `wine/d7vk-ddraw` (also D3D8/9 for i386 programs) and `feat/d7vk-ddraw` (`stage-dxvk.sh`, `build-dxvk-macos.sh`). MetalSharp Wine x86_64: `source/wine-x86_64` `feat/d7vk-ddraw`. MetalSharp: `feat/d7vk-ddraw` (`build-wine-x86_64-app.sh`) |

`tools/smoke/ddraw_run.sh ... routed`, with each Wine's patched `ntdll.so` in a copy of its installed app and this
branch's DLLs as its lane, 2026-11-21:

| Wine | Program | `routed` (`GRAPHICS_BACKEND=dxvk`) | `default` |
|---|---|---|---|
| MetalSharp Wine (arm64, FEX) | i386 | 8/8, d7vk | 8/8, Wine's ddraw |
| MetalSharp Wine (arm64, FEX) | x86_64 | 8/8, d7vk | 8/8, Wine's ddraw |
| MetalSharp Wine x86_64 | i386 | 8/8, d7vk | 8/8, Wine's ddraw |
| MetalSharp Wine x86_64 | x86_64 | 8/8, d7vk | 8/8, Wine's ddraw |

`GRAPHICS_BACKEND=wine` keeps Wine's ddraw too. On MetalSharp Wine, `d3d9_draw.c` as an i386 program with only
`GRAPHICS_BACKEND=dxvk` passes 8/8 on this branch's i386 D3D9.

The loader only loads builtins that have a file in the prefix, so `ddraw_.dll` must ship in `lib/wine/<arch>` for
wineboot to install. Because that file's export name is `ddraw.dll`, the loader also has to keep it away from the
lane (`is_d7vk_proxy_file()`); without that, d7vk loads itself as its proxy and overflows the stack. On the FEX build the
MetalSharp app already sends 32-bit DXVK games (`dxvk_32`, through `d3d9`) to `GRAPHICS_BACKEND=dxvk`, which the old
loader turned back into Mullion for i386 programs; MetalSharp's `feat/d7vk-ddraw` also maps a bare `dxvk_32` to
`dxvk`. Still open: `HKCU\Software\Wine\Direct3D\renderer=gdi` with d7vk is untested.

# Load time and encoding costs

Branch re-parity on main 6a1326c. Apple M6, macOS 27.3 (26C5035r); x86_64 Wine 11.17 (plain build, no lane routing); DXC x64 1.9.2609 for the harness's DXIL shaders.

- libMoltenVK.dylib sha256 00469cef8dc4146d36d47b630aba7a88045f69c004569bf99f28d2a1845a3e69
- d3d12core.dll sha256 6ad725f57136159014ca08f7a43b64ef8d27c87db724aef6501d15dc89cafb03
- MoltenVK commits: 9076dbf2 9e01af6d a3fccb86 68703541 2bd7597e deea24c2 3b3af2ae 1726012b 
- vkd3d-proton commits: b36f9e85 eaa12f9a 

## Functional harness (tests/d3d12/d3d12test.cpp, all tests)

main and this branch alike: 139 PASS, 17 FAIL, 0 failed checks. The 17 are DXBC compiles Wine's d3dcompiler_47 can't do here plus two that need a swap chain (uav_barrier_in_render_pass, d3d11on12); the same 17 on both.

```
[uav_barrier_in_render_pass] FAIL
[tess_domains_dxbc] FAIL
[gs_features_dxbc] FAIL
[buffer_robustness_dxbc] FAIL
[dispatch_ids_dxbc] FAIL
[gs_topologies_dxbc] FAIL
[gs_outputs_dxbc] FAIL
[gs_multistream_dxbc] FAIL
[raster_clip_dxbc] FAIL
[ia_restart_dxbc] FAIL
[ps_depth_stencil_out_dxbc] FAIL
[interp_samples_dxbc] FAIL
[dxbc_stripped] FAIL
[raster_vp_lines_dxbc] FAIL
[compute_limits_dxbc] FAIL
[d3d11on12] FAIL
[view_kinds_dxbc] FAIL
```

## Converted shaders on disk

The harness's 149 bridged shaders: 1.006 ms each to convert, 0.059 ms each from disk (MVK_CONFIG_MSC_DISK_CACHE=0 against 1, warm otherwise).

## Perf probe (tests/perf/d3d12_perf_probe.cpp)

Base: the MetalSharp Wine 7.0 lane (d3d12/d3d12core and libMoltenVK of main). Each scene in its own process, alternating lanes, best of three (five for the second set) by median CPU time; values are medians in ms per frame unless named.

```
compute: cpu 1.215->1.218 (+0%); work 1.209->1.212 (+0%); gpu 2.399->2.455 (+2%); first 401.038->418.969 (+4%); init_ms 26.448->51.141 (+93%); record_us_per_dispatch 0.190->0.191 (+1%)
alias_dispatches: cpu 0.214->0.209 (-2%); work 0.211->0.204 (-3%); gpu 0.523->0.533 (+2%); first 424.122->387.892 (-9%); init_ms 29.307->22.863 (-22%)
uav_barriers: cpu 0.129->0.125 (-3%); work 0.126->0.121 (-4%); gpu 0.333->0.328 (-2%); first 506.542->399.113 (-21%); init_ms 118.227->26.369 (-78%)
barriers: cpu 0.171->0.160 (-6%); work 0.165->0.153 (-7%); gpu 0.397->0.376 (-5%); first 386.519->412.095 (+7%); init_ms 33.832->25.456 (-25%); record_us_per_barrier 0.093->0.094 (+1%)
descriptors: cpu 0.717->0.720 (+0%); work 0.707->0.710 (+0%); gpu 0.291->0.288 (-1%); first 444.272->382.687 (-14%); init_ms 32.487->26.796 (-18%); us_per_copy 0.055->0.056 (+1%); us_per_view 0.090->0.089 (-1%)
execute_indirect: cpu 0.405->0.415 (+2%); work 0.403->0.409 (+1%); gpu 0.781->0.769 (-2%); first 411.284->399.691 (-3%); init_ms 31.902->26.820 (-16%)
pso_cold: cpu 12.446->8.039 (-35%); work 12.441->8.034 (-35%); gpu 0.711->1.720 (+142%); first 492.969->549.372 (+11%); create_ms_per_pso 1.913->0.722 (-62%); hlsl_compile_ms_per_pso 1.127->1.194 (+6%); init_ms 32.245->29.432 (-9%)
pso_cold_threads: cpu 13.976->8.002 (-43%); work 13.963->7.997 (-43%); gpu 0.710->7.467 (+952%); first 510.787->470.341 (-8%); create_ms_per_pso 5.338->1.992 (-63%); init_ms 30.594->25.117 (-18%); wall_ms_per_frame 12.256->7.310 (-40%)
pso_warm: cpu 4.575->4.620 (+1%); work 4.570->4.618 (+1%); gpu 0.563->0.545 (-3%); first 520.591->375.208 (-28%); create_ms_per_pso 0.105->0.073 (-31%); init_ms 30.537->24.588 (-19%)
unity_frame: cpu 1.027->1.036 (+1%); work 1.022->1.028 (+1%); gpu 1.558->1.562 (+0%); first 518.219->481.294 (-7%); init_ms 179.925->138.269 (-23%); record_ms 0.230->0.221 (-4%)
passes: cpu 2.606->2.681 (+3%); work 2.092->2.678 (+28%); gpu 4.908->3.846 (-22%); first 174.007->106.114 (-39%); init_ms 160.598->99.142 (-38%); record_us_per_pass 2.463->1.285 (-48%)
empty: cpu 0.097->0.098 (+1%); work 0.087->0.092 (+6%); gpu 0.263->0.359 (+37%); first 395.405->393.477 (-0%); init_ms 29.744->31.932 (+7%)
execute_indirect: cpu 0.399->0.151 (-62%); work 0.396->0.150 (-62%); gpu 0.791->0.432 (-45%); first 108.538->28.476 (-74%); init_ms 101.508->23.135 (-77%)
empty: cpu 0.095->0.093 (-2%); work 0.092->0.092 (+0%); gpu 0.279->0.272 (-3%); first 104.252->65.315 (-37%); init_ms 99.787->59.877 (-40%)
unity_frame: cpu 1.039->1.014 (-2%); work 1.031->1.008 (-2%); gpu 1.650->1.616 (-2%); first 48.814->37.060 (-24%); init_ms 42.512->30.849 (-27%); record_ms 0.247->0.261 (+6%)
```

perf-1 was measured before the ExecuteIndirect commit, perf-2 after it. In pso_cold and pso_cold_threads the gpu column rises because the first draw now waits for a compile that overlapped the frame; wall_ms_per_frame is the end-to-end time.

## Later changes (same branch)

Functional harness after each: 141 pass (139 plus `executeindirect_count_draws` and `executeindirect_count_dispatches`),
17 fail (the same 17), 0 failed checks. Perf probe, best or typical of three, x86_64 under a plain Wine on an M6:

| Change | Scene | Before | After |
|---|---|---|---|
| SyncInterval as a relative present target | `present_interval2`, 144 Hz | 6.96 ms/frame | 13.89 ms/frame (2 refreshes) |
| Count-buffer draws from an ICB | `execute_indirect_count_draws` (4000 max, 200 counted) | CPU 0.37, GPU 0.78 ms | CPU 0.19, GPU 0.50 ms |
| (same, with root constants) | `execute_indirect_count` (4000 max, 2000 counted) | CPU 0.90, GPU 1.74 ms | CPU 0.25, GPU 0.61 ms |
| Dispatch-only count buffers via generated commands | `execute_indirect_dispatch_only` | CPU 2.70, GPU 5.8 ms | CPU 0.58, GPU 1.0 ms |
| Generated draws' kernels before the render pass | `gbuffer_execute_indirect` | CPU 1.08, GPU 1.90 ms | CPU 0.49, GPU 1.11 ms |
| Tessellation in the converter's emulation | `tess_dxil_many` (100 draws of 10 patches) | CPU 4.35, GPU 6.3 ms (SPIR-V) | CPU 0.42, GPU 0.73 ms |
| Command lists encode on their recording threads | `mt_submit` (8 threads x 500 draws) | CPU 0.81, GPU 1.31 ms | CPU 0.48, GPU 0.71 ms |
| (same) | `many_lists` (40 lists) | GPU 2.41 ms | GPU 0.75 ms |
| Direct fence waits | `empty`, median submit-to-completion | 0.299 ms | 0.276 ms |

"GPU" is the probe's serialized submit-to-completion figure. "Before" is the MetalSharp Wine 7.0 lane except where
it says SPIR-V (the same build with `VKD3D_MSC_TESSELLATION=0`).

## Coverage of the D3D12 translation's mechanisms

| Area | Here |
|---|---|
| Command lists, queues, fences | encoding on recording threads; direct fence waits; one queue family (MoltenVK's single-queue semaphores) |
| Barriers | encoder-preserving barriers; enhanced barriers take ResourceBarrier's shortcuts |
| Resources, heaps, residency | residency set; GPU-side zero init; host-only CPU descriptor heaps |
| Descriptors | converter top-level argument buffers with shadow heaps; per-resource view cache (VKD3D-Proton's); RGB32 typed buffers stay on SPIR-V (no converter support) |
| PSOs and shaders | converter conversions on disk and in memory; deferred, archived compiles; DXBC converted once |
| Draw, dispatch, ExecuteIndirect | root arguments into the top-level argument buffer; ICBs for count buffers; kernels before the pass |
| Tessellation, geometry, mesh | tessellation in the converter's emulation (not isolines, DXBC or with a geometry shader); mesh shaders converted |
| Queries, predication | predication by patched indirect arguments, as the reference does outside its private layer |
| Ray tracing | residency instead of per-structure useResource |
| Clear, copy, resolve | clears folded into load actions (VKD3D-Proton's) |
| Swap chain, MetalFX | relative present targets for SyncInterval; asynchronous scaler creation |
| Caps | options to report no ray tracing before Apple family 9 and no conservative rasterization |

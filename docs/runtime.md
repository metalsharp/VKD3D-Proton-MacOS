# Runtime behaviour and switches

How the runtime compiles pipelines, runs ExecuteIndirect, presents and converts shaders, and the environment variables
that change each. Every switch below is optional: the defaults are what the runtime is tested with.

## Configuration files

The arm64 package ships `vkd3d.conf` and `dxvk.conf`. The installer copies them into `<prefix>/vkd3d-proton-macos/`
(a reinstall keeps your edits), and `env.sh` points `VKD3D_CONFIG_FILE` and `DXVK_CONFIG_FILE` at them. MetalSharp
writes its own; other Wines use these. A path in either variable may be a Unix path or a Windows one (`Z:/...`).

### MetalFX upscaling

MetalFX spatial upscaling scales the swap chain to the display where a game renders smaller than its window. It is on
in `vkd3d.conf` (`d3d12.metalSpatialUpscale`) and `dxvk.conf` (`d3d11.metalSpatialUpscale`); `VKD3D_METALFX_SPATIAL=1`
or `0` overrides the file for D3D12.

### DLSS

DLSS Super Resolution, Ray Reconstruction and Frame Generation run on MetalFX. The package's NGX core (`nvngx.dll`,
signed with the Mullion NGX key, whose certificate the lane's DXGI trusts in the prefix) and NVAPI answer only games
that DXGI shows an NVIDIA GPU: uncomment the `dxgi.customVendorId = 10de` lines in `dxvk.conf` under the game's
`[Game.exe]` section.

Ray Reconstruction and Frame Generation need the game to ship `nvngx_dlssd.dll` and `nvngx_dlssg.dll`, as on Windows.
Frame Generation needs macOS 27.

## Pipeline compilation

Creating a pipeline returns once its shaders are converted. Metal compiles it meanwhile, started from the creating
thread and finished by its completion handler, so no thread waits for it; the first draw or dispatch that uses the
pipeline waits if it isn't done.

| Variable | Effect |
| --- | --- |
| `MVK_CONFIG_DEFERRED_PIPELINES=0` | Compile in the creating call instead. |
| `MVK_CONFIG_PARALLEL_STAGE_COMPILE=1` | Compile a pipeline's fragment shader alongside its vertex shader. Off by default, as its compiles wait on Metal from GCD worker threads. |
| `MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION=0` | Don't have Metal compile with as many threads as the machine has (the default). |

## Metal Shader Converter

Apple's Metal Shader Converter turns DXIL and DXBC shaders straight into Metal libraries for most pipelines (compute;
vertex, mesh, amplification and pixel), skipping SPIR-V and Metal source.

Tessellation pipelines of DXIL shaders run in the converter's emulation: a mesh pipeline whose object stage runs the
vertex and hull shaders and whose mesh stage runs the tessellator and the domain shader, so a tessellated draw no
longer ends the render pass for compute passes. Isolines, DXBC tessellation and ExecuteIndirect with such a PSO use
SPIR-V.

These stay on SPIR-V: geometry shaders, tessellation with a geometry shader, stream output, logic operations,
conservative rasterization, the depth bounds test, view instancing, ray queries, 64-bit atomics, barycentrics,
residency feedback, min/max sampler filtering, and three-component 32-bit typed buffers.

Converted shaders are kept on disk beside MoltenVK's pipeline cache (`<cache>.msc/`, one file each), so a relaunch
converts none of them again. The converter targets the GPU's Apple family and the running macOS.

| Variable | Effect |
| --- | --- |
| `METALSHARP_MSC=0` | Keep every pipeline on SPIR-V. |
| `METALSHARP_MSC=compute` | Convert compute pipelines only; graphics ones stay on SPIR-V. |
| `METALSHARP_MSC_DXBC=0` | Keep DXBC (SM 4.x and 5.x) shaders on SPIR-V. |
| `VKD3D_MSC_TESSELLATION=0` | Keep tessellation pipelines on SPIR-V. |
| `MVK_CONFIG_MSC_DISK_CACHE=0` | No disk cache of converted shaders. |
| `MVK_CONFIG_MSC_TARGET_DEVICE=0` | Target the converter's oldest devices and macOS instead of the running ones. |

## ExecuteIndirect and indirect draws

- ExecuteIndirect of 25 or more draws whose signature has only root constants runs from one Metal indirect command
  buffer.
- ExecuteIndirect that sets root constants or root descriptors writes them into a converted pipeline's top-level
  argument buffer, so the pipeline stays the converter's instead of switching to a SPIR-V one compiled for it.
- An indirect draw with a count buffer of 25 or more draws (ExecuteIndirect of draws alone) runs only the counted
  draws, from an indirect command buffer encoded before the render pass. ExecuteIndirect of dispatches alone with a
  count buffer runs only the counted dispatches.
- ExecuteIndirect inside a render pass doesn't end it: its kernels run before the pass begins.

| Variable | Effect |
| --- | --- |
| `MVK_CONFIG_DGC_RENDER_ICB_MIN_SEQUENCES` | Draw count from which root-constant ExecuteIndirect uses an indirect command buffer (default 25; `0` turns it off). |
| `MVK_CONFIG_INDIRECT_COUNT_ICB_MIN_DRAWS` | The same threshold for count-buffer draws (default 25; `0` turns it off). |
| `MVK_CONFIG_DGC_HOIST=0` | Run ExecuteIndirect's kernels in place, ending the render pass. |

## Command encoding and MetalFX

Command lists encode into Metal when their recording ends, on the thread that recorded them, not all at once on the
submitting thread. MetalFX's temporal scaler is made without stopping the queue.

| Variable | Effect |
| --- | --- |
| `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=0` | Encode command lists at submission. |
| `MVK_CONFIG_METALFX_SYNCHRONOUS_INIT=1` | Wait for MetalFX's temporal scaler to be made. |
| `MVK_CONFIG_METALFX_OUTPUT_MOTION=1` | Let MetalFX take output-resolution motion vectors itself (macOS 27), instead of a downscale pass. |

## Presentation

`Present` with a SyncInterval of 2 to 4 holds each frame for that many refreshes (through relative present targets),
so a game capped at half or a third of the refresh rate stays capped.

## Reported features

Two options leave out features that run slowly here, for games that pick a faster path without them:

| Variable | Effect |
| --- | --- |
| `MVK_CONFIG_RAY_TRACING_MIN_APPLE_FAMILY=9` | Report no ray tracing on GPUs before Apple family 9, which trace rays in compute. |
| `MVK_CONFIG_CONSERVATIVE_RASTERIZATION=0` | Report no conservative rasterization, which is emulated. D3D12 feature level 12_1 needs it, so 12_0 is reported. |
| `MVK_CONFIG_ENABLE_EXPERIMENTAL_RAY_TRACING=1` | Turn on MoltenVK's ray tracing pipelines: feature level 12_2 and DXR 1.1. |
| `METALSHARP_METAL_VERSION=3` | Run MoltenVK as on macOS 14 and 15 (no MSL 4.0, Metal 4 family features, sampler reduction or sparse resources), to test the Metal 3 paths on a newer system. |

## CPU paths on Apple Silicon

The arm64 package's ARM64X builds have no SSE2, so VKD3D-Proton's and DXVK's SIMD paths run on NEON there: descriptor
copies (CopyDescriptors finds each descriptor set's runs with bit scans), non-temporal descriptor writes (STNP), the
Metal Shader Converter's per-draw root argument pushes, spin waits (`isb`), D3D9 constant updates and d7vk's software
vertex processing.

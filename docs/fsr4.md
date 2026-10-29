# AMD FSR Upscaling 4 on VKD3D-Proton and MoltenVK

`METALSHARP_FSR4=1` lets games that ship AMD FSR Upscaling 4 (the FSR SDK 2.x DLLs, such as FSR 4.1.2 from SDK 2.4)
offer FSR 4 on an Apple GPU. It is **off by default**, and with it unset nothing below changes anything.

It works, and on the test scene below FSR 4 gives the best image of the three FSR versions. But it is far too slow to
play with on an Apple GPU: **177 ms per frame at 1080p output on an M4**, against 11 ms for FSR 3.1. See
[Performance](#performance) before turning it on.

## Turning it on and off

| Setting | Effect |
| --- | --- |
| `METALSHARP_FSR4` unset, or anything but `1` | Off. Games see the real GPU and FSR offers 3.1 and 2.3 as before. |
| `METALSHARP_FSR4=1` | On. A game's FSR SDK 2.x DLLs offer FSR 4 as well, and pick it by default. |
| `METALSHARP_FSR4_LOG=1` | `amdxc64.dll` logs each request from FSR's DLLs to stderr. |

Set it in the game's environment (for example in a launch option or `launch.env`) and restart the game. A game reads
the GPU and FSR's version list once, at start.

## What it changes

AMD's FSR DLLs only offer FSR 4 on an AMD GPU whose driver is loaded. With the setting on, four parts each answer one
of their checks:

1. **VKD3D-Proton** loads `amdxc64.dll` when it creates a D3D12 device under Wine, on any GPU. Upstream loads it only
   on an AMD GPU (`d3d12_device_init_vendor_hacks()`, patch in `patches/vkd3d-proton-moltenvk.patch`).
2. **`amdxc64.dll`** (`amdxc64/` in this repository) stands in for that driver DLL, whose name FSR's providers look
   for. It exports `AmdExtD3DCreateInterface` and answers the extension interfaces they ask for:
   - `IAmdExtFfxApi`: no driver provider of FSR (FSR uses the game's own DLLs);
   - `IAmdExtD3DFactory`: an `IAmdExtD3DDevice8` that reports no wave matrix (WMMA) support, so FSR runs its INT8
     model, and declines the rest;
   - Anti-Lag: forwarded to VKD3D-Proton's device, which implements it.

   With `METALSHARP_FSR4` unset it declines everything. It builds on Proton's `amdxc64` (Wine `dlls/amdxc64`,
   Etaash Mathamsetty) and is LGPL-2.1-or-later like it.
3. **DXVK's DXGI** ([metalsharp/DXVK-MacOS](https://github.com/metalsharp/DXVK-MacOS)) reports the adapter as an AMD
   Radeon RX 7900 XTX (vendor `1002`, device `744c`), unless `dxgi.customVendorId` or `dxgi.customDeviceId` are set.
   The adapter's name stays the real one.
4. **MetalSharp Wine** loads the lane's `amdxc64.dll` only with the setting on, and answers the D3DKMT query for AMD's
   driver data (`KMTQAITYPE_UMDRIVERPRIVATE`) with a Navi31 (RDNA 3) GPU family and revision, which selects FSR 4's INT8
   model. Proton answers the same. Another Wine needs that query answered, or FSR's provider declines.

The game sees the AMD GPU too. A game may then take AMD-specific paths, such as AMD's AGS library, which VKD3D-Proton
implements. Turn the setting off if a game misbehaves with it.

### What isn't covered

- **Games with FSR 3.1 DLLs** (FSR SDK 1.x, `amd_fidelityfx_dx12.dll`) don't get FSR 4. On Windows, AMD's driver
  upgrades them through `IAmdExtFfxApi`, and this `amdxc64.dll` doesn't provide that. Given the performance below,
  an upgrade would only make those games slower.
- **The WMMA (FP8) model**, which FSR 4 runs on RDNA 4. It needs 16×16 cooperative matrices with AMD's layouts.
  MoltenVK has none, and Metal's `simdgroup_matrix` is 8×8 with no FP8.
- **Frame generation** is unchanged.

## Performance

`tests/fsr/fsr4probe.cpp` on an Apple M4 (10-core GPU), 960×540 → 1920×1080 (performance mode), 40 frames of a moving,
jittered test scene. PSNR is the last output frame against the scene rendered at 1920×1080.

| Upscaler | Time per dispatch | PSNR | Bilinear upscale |
| --- | ---: | ---: | ---: |
| FSR 4.1.2 (INT8 model) | 177 ms | 33.83 dB | 26.28 dB |
| FSR 3.1.6 | 11.4 ms | 33.35 dB | 26.28 dB |
| FSR 2.3.4 | 10.1 ms | 32.92 dB | 26.28 dB |

FSR 4's INT8 network does most of its work in packed 8-bit dot products (HLSL `dot4add_i8packed`). AMD GPUs run those
in one instruction; Apple GPUs have no 8-bit dot product, so each one becomes byte conversions and a float dot
product. FSR 4 is then about 15× slower than FSR 3.1 on the same GPU. Larger Apple GPUs scale roughly with their core
count, so even an M4 Max would stay well above a 16 ms frame. Treat the setting as a way to try FSR 4's image quality,
not as a way to play.

### What was optimized

- **The dot products.** SPIRV-Cross used to widen each byte to a 32-bit integer and multiply (245 ms per frame). It now
  converts the bytes to float and multiplies there (177 ms). The result is exact: every product and every partial
  sum is an integer below 2^24. `tests/fsr/i8dotcheck.m` checks every byte pair in every lane under each of Metal's
  math modes, and `tests/fsr/i8dotbench.m` measures the formulations.
- **Not used: the unorm8 unpack.** Unpacking the bytes as unorm8 is faster still (112 ms) and exact on its own. But in
  FSR 4's shaders it changed the output: more speckles and a jagged edge, 33.04 dB. That happened even in a build
  that computed both forms and returned the integer one. So the change comes from how Metal's compiler builds the
  rest of the shader around that instruction, not from the values. Fast math, forced threadgroup barriers and
  zero-initialized variables made no difference.
- **Shader path.** 49 of FSR's 50 pipelines go through the Metal Shader Converter bridge: the same output as on
  SPIR-V, 175 ms, and milliseconds instead of seconds to build each pipeline. The one shader that uses both wave
  operations and 16-bit types drew wrong there (30 dB instead of 34 after 12 frames, found by moving shaders to the
  bridge by hash range: `METALSHARP_MSC_HASHES`), so shaders with both stay on SPIR-V.

## Testing

```bash
scripts/fetch-fsr-sdk.sh                                   # FSR SDK 2.4.0's signed DLLs and API headers, checksummed
LLVM_MINGW=<llvm-mingw> scripts/build-fsr-probe.sh         # artifacts/build/fsr-probe: fsr4probe.exe + AMD's DLLs
LLVM_MINGW=<llvm-mingw> scripts/build-amdxc64.sh           # artifacts/build/amdxc64/{x86_64-windows,arm64x}
```

Run `fsr4probe.exe` from `artifacts/build/fsr-probe` with this package's DLLs installed and `METALSHARP_FSR4=1`. It
lists the upscaler versions FSR offers (4.1.2, 3.1.6 and 2.3.4 when FSR 4 is on; 3.1.6 and 2.3.4 when it is off) and
upscales with each. It checks each output against the scene, and fails when one is further from it than a bilinear
upscale. Options: `--version <substring>`, `--frames N`, `--mode quality|balanced|performance|ultra`, `--size WxH`,
and `--images <dir>` for PPM images of the output, the scene and the bilinear upscale.

To check that the toggle is off, run the probe without the setting: only 3.1.6 and 2.3.4 are offered, and
`amdxc64.dll in the process: no`.

## AMD's DLLs and license

AMD's FSR DLLs are AMD's signed binaries, under the FSR SDK's license (`Kits/FidelityFX/docs/license.md` in the
SDK). It allows redistributing them unmodified, in binary form, with AMD's notices. It forbids reverse engineering,
decompiling and disassembling them.

- No release package here ships them. Games ship their own, and `scripts/fetch-fsr-sdk.sh` downloads the SDK's copies
  for the probe, with the license next to them.
- Nothing here modifies, inspects or disassembles them. That includes the shaders they create: don't dump FSR's shaders
  (`METALSHARP_TRACE=shader+heavy`, `MVK_CONFIG_SHADER_DUMP_DIR`, VKD3D-Proton's shader dumps) or read their SPIR-V or
  MSL. Optimize with timings and the probe's image checks instead, as above.
- The FFX API headers the probe builds against are MIT-licensed.

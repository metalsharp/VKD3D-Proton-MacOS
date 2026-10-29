# AMD FSR 4 on VKD3D-Proton and MoltenVK

`METALSHARP_FSR4=1` lets games that ship AMD FSR 4 (the FSR SDK 2.x DLLs, such as FSR Upscaling 4.1.2, FSR Frame
Generation 4.0.2 and its swapchain 4.0.0 from SDK 2.4) offer them on an Apple GPU. It is **off by default**, and with
it unset nothing below changes anything.

They work: FSR 4's networks run on Apple's matrix hardware (`simdgroup_matrix`) through cooperative matrices in
MoltenVK, and on the test scene below FSR Upscaling 4 gives the best image of the three FSR versions. But they are far
too slow to play with on an Apple GPU: **186 ms per upscaled frame and 76 ms per generated frame at 1080p on an M4**,
against 10 ms and 23 ms for FSR 3.1. See [Performance](#performance) before turning it on.

## Turning it on and off

| Setting | Effect |
| --- | --- |
| `METALSHARP_FSR4` unset, or anything but `1` | Off. Games see the real GPU and FSR offers 3.1 and 2.3 as before. |
| `METALSHARP_FSR4=1` | On. A game's FSR SDK 2.x DLLs offer FSR 4 as well, and pick it by default: Upscaling 4 with its FP8 model, Frame Generation 4.0.2 (on Windows 11, see below) and its swapchain 4.0.0. |
| `METALSHARP_FSR4_GPU=rdna3` | With `METALSHARP_FSR4=1`: the GPU answers as RDNA 3 instead of RDNA 4. Upscaling 4 runs its INT8 model, which needs no matrices (as on Proton), and Frame Generation stays at 3.1. |
| `METALSHARP_FSR4_LOG=1` | `amdxc64.dll` logs each request from FSR's DLLs to stderr; any other value is a file to append them to (`C:\fsr.log`). |

FSR Frame Generation 4.0.2 is only offered when the prefix reports Windows 11 (`winecfg`, Applications, Windows
Version). Upscaling 4 and the 4.0.0 swapchain don't need it.

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
   - `IAmdExtD3DFactory`: an `IAmdExtD3DDevice8` that reports 16×16×16 FP8 wave matrices (WMMA) when VKD3D-Proton
     runs them natively, and `IAmdExtD3DShaderIntrinsics`, which answers which of AMD's shader intrinsics (AGS) shaders
     may use, as VKD3D-Proton's shader compiler implements them (`ID3D12DeviceExt3::SupportsAGSExtension`). Pipeline
     creation from AMD shader binaries is declined;
   - Anti-Lag: forwarded to VKD3D-Proton's device, which implements it.

   With `METALSHARP_FSR4` unset it declines everything. It builds on Proton's `amdxc64` (Wine `dlls/amdxc64`,
   Etaash Mathamsetty) and is LGPL-2.1-or-later like it.
3. **DXVK's DXGI** ([metalsharp/DXVK-MacOS](https://github.com/metalsharp/DXVK-MacOS)) reports the adapter as an AMD
   Radeon RX 7900 XTX (vendor `1002`, device `744c`), unless `dxgi.customVendorId` or `dxgi.customDeviceId` are set.
   The adapter's name stays the real one.
4. **MetalSharp Wine** loads the lane's `amdxc64.dll` only with the setting on, and answers the D3DKMT query for AMD's
   driver data (`KMTQAITYPE_UMDRIVERPRIVATE`) with a Navi48 (RDNA 4) GPU family and revision (Navi31, RDNA 3, with
   `METALSHARP_FSR4_GPU=rdna3`, as Proton answers). FSR's providers decline without that answer; RDNA 4 is what offers
   Frame Generation 4 and its swapchain. Wine patches 0091 to 0094.
5. **MoltenVK** exposes `VK_KHR_cooperative_matrix` and `VK_EXT_shader_float8` on Apple 7 GPUs and later (M1 on),
   which VKD3D-Proton needs for AMD's WMMA intrinsics. SPIRV-Cross builds 16×16 matrices from 8×8
   `simdgroup_matrix` tiles in Metal's layout, gives shaders their components in the order RDNA 4's WMMA instructions
   use (FSR's shaders depend on it), and holds FP8 values as half, rounded to E4M3 as AMD's conversions do. Shaders
   with cooperative matrices, and pipelines with AMD's AGS register space, stay on SPIR-V rather than the Metal Shader
   Converter bridge.

The game sees the AMD GPU too. A game may then take AMD-specific paths, such as AMD's AGS library, which VKD3D-Proton
implements. Turn the setting off if a game misbehaves with it.

### What isn't covered

- **Games with FSR 3.1 DLLs** (FSR SDK 1.x, `amd_fidelityfx_dx12.dll`) don't get FSR 4. On Windows, AMD's driver
  upgrades them through `IAmdExtFfxApi`, and this `amdxc64.dll` doesn't provide that. Given the performance below,
  an upgrade would only make those games slower.
- **Hardware FP8.** Apple GPUs multiply half, float and bfloat matrices; FP8 values are widened to half, which holds
  every E4M3 value exactly, so results match but nothing is gained from FP8's smaller size.

## Performance

`tests/fsr/fsr4probe.cpp` on an Apple M4 (10-core GPU), 960×540 → 1920×1080 (performance mode), 40 frames of a moving,
jittered test scene. PSNR is the last output frame against the scene rendered at 1920×1080.

| Upscaler | Time per dispatch | PSNR | Bilinear upscale |
| --- | ---: | ---: | ---: |
| FSR 4.1.2, FP8 model (default) | 186 ms | 33.78 dB | 26.28 dB |
| FSR 4.1.2, INT8 model (`METALSHARP_FSR4_GPU=rdna3`) | 175 ms | 33.83 dB | 26.28 dB |
| FSR 3.1.6 | 10.3 ms | 33.33 dB | 26.28 dB |
| FSR 2.3.4 | 9.3 ms | 32.93 dB | 26.28 dB |

Frame generation, 1920×1080, the same scene without jitter, 40 frames; each generated frame is compared with the scene
halfway between the two it was generated from, alongside repeating the earlier frame (27.08 dB) and a 50% blend of
the two (31.89 dB). The time covers PrepareV2 and generation in one submission.

| Frame generation | Time per generated frame | PSNR |
| --- | ---: | ---: |
| FSR Frame Generation 4.0.2 | 76 ms | 33.00 dB |
| FSR Frame Generation 3.1.6 | 23 ms | 38.96 dB |

Both models run far slower than FSR 3.1. The INT8 network does most of its work in packed 8-bit dot products (HLSL
`dot4add_i8packed`): one instruction on AMD GPUs, byte conversions and a float dot product on Apple GPUs, which have no
8-bit dot product. The FP8 network runs its matrix multiplies on Apple's matrix hardware, but in this test about half
of its time goes to moving matrix components between SIMD lanes: FSR's shaders read and write components in RDNA 4's
order, which differs from Metal's matrix layout. Larger Apple GPUs scale roughly with their core count, so even an M4
Max would stay well above a 16 ms frame. Frame Generation 4.0.2 uses the same matrix intrinsics (FSR offers it only
with them), and on this scene it is also less accurate than 3.1.6. Treat the setting as a way to try FSR 4, not as a
way to play.

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
- **Cooperative matrix storage.** At first, the FP8 model took 9.2 s per frame. SPIRV-Cross kept each 8×8 tile as a
  `simdgroup_matrix` and wrote its components through `thread_elements()`, which Metal treats as a 64-wide vector:
  writing components of any but the first tile put the matrices in memory (Instruments showed over 100 KB of spills per
  dispatch). Tiles are now each invocation's two components in an ordinary vector, turned into `simdgroup_matrix` only
  for Metal's multiplies, loads and stores, where the conversion costs nothing: 185 ms. This was found by timing
  variants of the generated code, not by reading FSR's shaders: stubbing a helper lets Metal's compiler remove all the
  work that feeds it, so each variant kept its inputs in use.
- **FP8 conversions.** Rounding to E4M3 works on the bits (add just under half a step, clear the dropped bits), and
  `tests/coopmat/fp8exh.m` checks it against a reference for every half and 4 million floats.
- **Shader path.** 49 of the INT8 model's 50 pipelines go through the Metal Shader Converter bridge: the same output
  as on SPIR-V, 175 ms, and milliseconds instead of seconds to build each pipeline. The one shader that uses both wave
  operations and 16-bit types drew wrong there (30 dB instead of 34 after 12 frames, found by moving shaders to the
  bridge by hash range: `METALSHARP_MSC_HASHES`), so shaders with both stay on SPIR-V.

## Testing

```bash
scripts/fetch-fsr-sdk.sh                                   # FSR SDK 2.4.0's signed DLLs and API headers, checksummed
LLVM_MINGW=<llvm-mingw> scripts/build-fsr-probe.sh         # artifacts/build/fsr-probe: fsr4probe.exe + AMD's DLLs
LLVM_MINGW=<llvm-mingw> scripts/build-amdxc64.sh           # artifacts/build/amdxc64/{x86_64-windows,arm64x}
```

Run `fsr4probe.exe` from `artifacts/build/fsr-probe` with this package's DLLs installed and `METALSHARP_FSR4=1`. It
lists the versions FSR offers (upscaling 4.1.2, 3.1.6 and 2.3.4, frame generation 4.0.2 and 3.1.6, swapchain 4.0.0
and 3.1.7 when FSR 4 is on in a Windows 11 prefix; the 3.1 and 2.3 ones when it is off) and upscales with each. It
checks each output against the scene, and fails when one is further from it than a bilinear upscale. Options:
`--version <substring>`, `--frames N`, `--mode quality|balanced|performance|ultra`, `--size WxH`, and
`--images <dir>` for PPM images of the output, the scene and the bilinear upscale.

`--fg` runs frame generation instead, without a swapchain, and checks each generated frame against the scene halfway
between its two frames. `--fg --swapchain` presents through the frame generation swapchain on a window it never shows,
and checks that the swapchain presented generated frames (twice as many presents as rendered frames).

`tests/coopmat` holds the Metal checks behind the cooperative matrices: `coopmat16.m` (Metal's 8×8 layout and
matrix throughput) and `fp8exh.m` (E4M3 rounding, on the helpers SPIRV-Cross emits:
`tests/coopmat/msl-helpers.py Float8 > fp8.metal`, then `fp8exh fp8.metal`).

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

<div align="center">

<h1>VKD3D-Proton macOS</h1>

<p>
  Reproducible D3D12-on-Metal runtime for Apple Silicon.
</p>

<p>
  <a href="https://github.com/metalsharp/vkd3d-proton-macos/releases/latest"><img src="https://img.shields.io/github/v/release/metalsharp/vkd3d-proton-macos?style=for-the-badge" alt="Release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-AGPL--3.0-blue.svg?style=for-the-badge" alt="AGPL-3.0 License"></a>
  <a href="https://github.com/metalsharp/vkd3d-proton-macos/actions/workflows/build.yml"><img src="https://img.shields.io/github/actions/workflow/status/metalsharp/vkd3d-proton-macos/build.yml?branch=main&style=for-the-badge&label=Build" alt="Build"></a>
</p>

<pre>D3D12 application → vkd3d-proton → Vulkan → custom MoltenVK → Metal
D3D8/9/10/11 application, D3D12 swap chains → DXVK (metalsharp/DXVK-MacOS) → Vulkan → custom MoltenVK → Metal</pre>

<p>
  Each <a href="https://github.com/metalsharp/vkd3d-proton-macos/releases/latest">release</a> has a package for
  x86_64 Wine, one for arm64 Wine with FEX, and the universal MoltenVK, with install scripts for both kinds of Wine.
</p>

</div>

## Capabilities

Measured on an Apple M4 (macOS 27.2) through VKD3D-Proton and this MoltenVK:

- Shader Model **6.8**
- D3D12 feature levels **12_1, 12_0, 11_1, 11_0** by default, and **12_2** with
  `MVK_CONFIG_ENABLE_EXPERIMENTAL_RAY_TRACING=1`, which turns on MoltenVK's ray tracing pipelines
- DXR **1.1** (ray tracing tier 1.1) with that setting; none without it
- Variable-rate shading tier 2 (16-pixel shading-rate tiles)
- Mesh shader tier 1, sampler feedback tier 0.9
- Tiled resources tier 4, conservative rasterization tier 3, resource binding tier 3
- ROVs, depth bounds, barycentrics, fully typed format casting, copy-queue timestamps, and output-merger logic
  operations

[docs/features.md](docs/features.md) has the full matrix and [docs/validation.md](docs/validation.md) the regression
gate.

## DXVK

Both VKD3D-Proton packages ship DXVK's `dxgi.dll`, `d3d11.dll`, `d3d10core.dll`, `d3d9.dll` and `d3d8.dll` from
[metalsharp/DXVK-MacOS](https://github.com/metalsharp/DXVK-MacOS), our fork of DXVK 3.1.1 for MoltenVK, at the release
tag `scripts/fetch-sources.sh` pins (`DXVK_REV`; each package names it in `DXVK-VERSION`). With them, one package
runs every Direct3D version from 8 to 12:

- **DXGI:** `dxgi.dll` creates the swap chains D3D12 games present with.
- **D3D10 and D3D11:** `d3d11.dll` and `d3d10core.dll` run D3D10 and D3D11 games, including D3D11 renderers in
  otherwise D3D12 games.
- **D3D11On12:** `d3d11.dll` provides `D3D11On12CreateDevice` on VKD3D-Proton's devices.
- **D3D9 and D3D8:** `d3d9.dll` and `d3d8.dll` run D3D9 and D3D8 games.

The x86_64 package builds them for x86_64. The arm64 package builds them as ARM64X, like its D3D12 DLLs, so that
under FEX DXVK runs natively too.

Over upstream DXVK, the fork adds:
- the ARM64X build;
- D3D11 query, D3D11On12 and DXGI fixes for MoltenVK;
- D3D9 render-target copies for Metal;
- a Metal HUD line with the API and feature level.

DXVK 3.1 depends on MoltenVK and SPIRV-Cross fixes that this repository carries (`patches/moltenvk`,
`patches/spirv-cross`), so use this release's MoltenVK with these DLLs. DXVK's own changes are made in
metalsharp/DXVK-MacOS, whose releases also have i386 builds for 32-bit programs.

## Install

Download a VKD3D-Proton package and the MoltenVK package from the latest release, extract them side by side, and run
the package's installer on your Wine prefix:

```bash
tar -xf vkd3d-proton-<tag>-x86_64.tar.zst      # or -arm64 for an arm64 Wine with FEX
tar -xf moltenvk-<tag>-universal.tar.zst
./vkd3d-proton-<tag>-x86_64/install-x86_64-wine.sh --prefix ~/my-prefix --wine /path/to/wine
~/my-prefix/vkd3d-proton-macos/wine game.exe
```

[docs/install.md](docs/install.md) covers both packages, the options, running programs and uninstalling.

## Build from source

```bash
git clone https://github.com/metalsharp/vkd3d-proton-macos.git
cd vkd3d-proton-macos
make help
make docs-check
```

For a complete local build, fetch the sources with `scripts/fetch-sources.sh`,
install the toolchain described in [the requirements guide](docs/requirements.md),
then run:

```bash
make tools       # verify the host and external tools
make vkd3d      # build d3d12.dll + d3d12core.dll
make moltenvk   # build the universal libMoltenVK.dylib
make flprobe    # build the D3D12 feature-level probe
```

The full target map is documented in [docs/Development.md](docs/Development.md). Do not
mix `d3d12.dll` and `d3d12core.dll` from different builds.

## Repository layout

```text
.
├── Makefile                         # build, probe, package, and doc targets
├── docs/                            # current guides plus dated evidence
├── install/                         # install scripts for x86_64 and arm64 Wine prefixes
├── patches/                         # this project's changes to VKD3D-Proton, MoltenVK and SPIRV-Cross
├── scripts/                         # environment, build, probe, and recovery tools
├── sources/                         # local source clones (intentionally ignored)
├── toolchain/                       # local llvm-mingw/toolchain assets
└── artifacts/                       # local builds, staging, and evidence
```

## Documentation

Start at [docs/README.md](docs/README.md), then see:

- [Installing into a Wine prefix](docs/install.md)
- [Requirements](docs/requirements.md)
- [Development and Make targets](docs/Development.md)
- [Feature-level matrix](docs/features.md)
- [Validation and regression](docs/validation.md)
- [Tracing a problem with `METALSHARP_TRACE`](docs/tracing.md)
- [Release and packaging](docs/release.md)
- [Final state](docs/Final.md)

## Project policies

- [Contributing](CONTRIBUTING.md)
- [Security policy](SECURITY.md)
- [Code of Conduct](CODE_OF_CONDUCT.md)

## License

This repository's own code, scripts and documentation are licensed under the
[GNU Affero General Public License v3.0](LICENSE).

The projects it builds keep their own licenses, and so do this repository's changes to them in `patches/`:
VKD3D-Proton is LGPL-2.1, DXVK is zlib, its dxbc-spirv shader compiler is MIT, and MoltenVK and SPIRV-Cross are
Apache-2.0. Each release package carries
the licenses of what it contains. [Third-party licenses](#third-party-licenses) lists them, with the terms Apple's
Metal Shader Converter library ships under.

## Third-party licenses

| Component | In the release | License | License file in the package |
| --- | --- | --- | --- |
| [VKD3D-Proton](https://github.com/HansKristian-Work/vkd3d-proton) | `d3d12.dll`, `d3d12core.dll` | LGPL-2.1 | `LICENSE.vkd3d-proton` |
| [DXVK](https://github.com/doitsujin/dxvk), as [metalsharp/DXVK-MacOS](https://github.com/metalsharp/DXVK-MacOS) | `dxgi.dll`, `d3d11.dll`, `d3d10core.dll`, `d3d9.dll`, `d3d8.dll` | zlib | `LICENSE.dxvk` |
| [dxbc-spirv](https://github.com/doitsujin/dxbc-spirv) | built into DXVK's DLLs | MIT | `LICENSE.dxbc-spirv` |
| [MoltenVK](https://github.com/KhronosGroup/MoltenVK) | `libMoltenVK.dylib` | Apache-2.0 | `LICENSE.MoltenVK` |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) | built into `libMoltenVK.dylib` | Apache-2.0 | `LICENSE.MoltenVK` (the same license) |
| Apple Metal Shader Converter 4.0 beta 3 | `libmetalirconverter.dylib` | Apple's license, see below | `LICENSE.metal-shaderconverter.rtf`, `ACKNOWLEDGEMENTS.metal-shaderconverter.rtf` |
| Metal Shader Converter headers | built into `libMoltenVK.dylib` | Apache-2.0 | `sources/MoltenVK/MoltenVK/MoltenVK/MetalShaderConverter/metal_irconverter/LICENSE.txt` in MoltenVK's source |

### Apple Metal Shader Converter

**What it is.** `libmetalirconverter.dylib` is Apple's Metal Shader Converter library, © Apple Inc. It is
proprietary, not open source, and is licensed under Apple's *Software License Agreement for Metal Shader Converter*.
The `moltenvk-vX.Y-universal` package ships that agreement and Apple's acknowledgements file next to the library.
The library is the only part of the Metal Shader Converter that this repository distributes: not its command-line
tool and not its documentation.

**What it is used for.** MoltenVK loads the library at runtime and calls its public C API, declared in the
Apache-2.0 headers above, to convert the DXIL shaders VKD3D-Proton hands it into Metal libraries. It converts
compute shaders, and the vertex and pixel shaders of graphics pipelines. Without it, every shader goes
DXIL → SPIR-V → MSL text, and Metal compiles that text at run time: 0.4–1.7 s per large shader, against 1–4 ms
for the converter. Shader conversion is all the library does here. `METALSHARP_MSC=0` turns it off. If the library
is missing or a conversion fails, VKD3D-Proton builds that pipeline from SPIR-V as before.

**Why we may use and ship it this way.** Our reading of Apple's agreement:

- **Section 2B** licenses the Dynamic Libraries (the converter's `.dylib` files) to be installed, used, tested and
  distributed "for the sole purpose of shader conversion". Converting D3D12 shaders for Metal is that purpose, and
  the library ships only for it.
- **Section 2E** forbids modifying, decompiling, disassembling or reverse engineering the software. The library
  ships byte for byte as Apple publishes it and keeps Apple's code signature: our scripts never re-sign it.
  `scripts/fetch-metal-shaderconverter.sh` checks its SHA-256 and Apple's signature. MoltenVK uses only the public
  API and the documented reflection the converter returns.
- **Sections 2C and 2D** limit the software to Apple-branded hardware and require copyright and proprietary notices
  to be kept. The library ships only in the macOS MoltenVK package, which runs only on Macs, with Apple's agreement
  and acknowledgements unmodified beside it. The library is not sold, rented or offered as a hosted service.
- **Section 7B** (and the agreement's opening note) asks that the software only be used with material you are
  permitted to use. It converts the shaders of the games you run, on your own Mac.
- Apple's own download requires an Apple ID, so this repository hosts the unmodified library, agreement and
  acknowledgements as the `deps-metal-shaderconverter-4.0b3` release, which section 2B's distribution grant allows
  ([docs/release.md](docs/release.md)).

Apple's agreement governs. If you redistribute the MoltenVK package, keep the library unmodified, with its agreement
and acknowledgements, and use it only for shader conversion on Apple-branded systems.

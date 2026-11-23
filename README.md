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

<pre>D3D12 application → VKD3D-Proton → Vulkan → MetalSharp's MoltenVK → Metal</pre>

</div>

___

VKD3D-Proton, built and patched for macOS, on a MoltenVK that runs it: Metal Shader Converter for most shaders,
MetalFX for upscaling and DLSS, and Metal 4 where the system has it. Its DXGI and Direct3D 11 come from
[DXVK-MacOS](https://github.com/metalsharp/DXVK-MacOS), the same DXVK as MetalSharp's DXVK lane.

Each [release](https://github.com/metalsharp/vkd3d-proton-macos/releases/latest) has three packages: one for x86_64
Wine, one for arm64 Wine with FEX (ARM64X, with NEON on the hot CPU paths), and the universal MoltenVK they share.

## Capabilities

Measured on an Apple M6 (macOS 27.2):

| | |
| --- | --- |
| Shader Model | **6.8** |
| Feature levels | **12_1** by default; **12_2** with `MVK_CONFIG_ENABLE_EXPERIMENTAL_RAY_TRACING=1` |
| Ray tracing | DXR **1.1** with that setting |
| Variable-rate shading | Tier 2 (16-pixel tiles) |
| Mesh shaders, sampler feedback | Tier 1, tier 0.9 |
| Tiled resources, conservative rasterization, resource binding | Tier 4, tier 3, tier 3 |
| Also | ROVs, depth bounds, barycentrics, fully typed format casting, copy-queue timestamps, logic operations |

The full matrix is in [docs/features.md](docs/features.md), and the regression gate in
[docs/validation.md](docs/validation.md).

## Install

Download a VKD3D-Proton package and the MoltenVK package from the latest release, extract them side by side, and run
the installer on your Wine prefix:

```bash
tar -xf vkd3d-proton-<tag>-x86_64.tar.zst      # or -arm64 for an arm64 Wine with FEX
tar -xf moltenvk-<tag>-universal.tar.zst
./vkd3d-proton-<tag>-x86_64/install-x86_64-wine.sh --prefix ~/my-prefix --wine /path/to/wine
~/my-prefix/vkd3d-proton-macos/wine game.exe
```

[docs/install.md](docs/install.md) covers both packages, the options, running programs and uninstalling.
[docs/runtime.md](docs/runtime.md) covers MetalFX, DLSS, and the switches for pipeline compilation, Metal Shader
Converter, ExecuteIndirect and presentation.

## Build from source

```bash
git clone https://github.com/metalsharp/vkd3d-proton-macos.git
cd vkd3d-proton-macos
scripts/fetch-sources.sh   # VKD3D-Proton, MoltenVK, DXVK-MacOS, NVAPI and DXC, with this repository's patches
make tools                 # check the host and the toolchain (docs/requirements.md)
make vkd3d                 # d3d12.dll and d3d12core.dll
make moltenvk              # the universal libMoltenVK.dylib
```

`make help` lists every target, and [docs/Development.md](docs/Development.md) explains them. Don't mix `d3d12.dll`
and `d3d12core.dll` from different builds.

```text
Makefile        build, probe, package and documentation targets
docs/           guides, and dated evidence
install/        installers for x86_64 and arm64 Wine prefixes
patches/        this project's changes to VKD3D-Proton, dxil-spirv, MoltenVK, SPIRV-Cross and DXC
scripts/        build, probe and packaging tools
sources/        source clones (ignored)
toolchain/      llvm-mingw and other local tools (ignored)
artifacts/      builds, staging and evidence
```

## Documentation

[docs/README.md](docs/README.md) is the index. The guides:
[install](docs/install.md) ·
[runtime](docs/runtime.md) ·
[requirements](docs/requirements.md) ·
[development](docs/Development.md) ·
[features](docs/features.md) ·
[validation](docs/validation.md) ·
[tracing](docs/tracing.md) ·
[release](docs/release.md) ·
[final state](docs/Final.md)

[Contributing](CONTRIBUTING.md) · [Security](SECURITY.md) · [Code of Conduct](CODE_OF_CONDUCT.md)

## License

This repository's own code, scripts and documentation are licensed under the
[GNU Affero General Public License v3.0](LICENSE). The projects it builds keep their own licenses, and so do this
repository's changes to them in `patches/`. Each release package carries the licenses of what it contains:

| Component | In the release | License | License file |
| --- | --- | --- | --- |
| [VKD3D-Proton](https://github.com/HansKristian-Work/vkd3d-proton) | `d3d12.dll`, `d3d12core.dll` | LGPL-2.1 | `LICENSE.vkd3d-proton` |
| [DXVK-MacOS](https://github.com/metalsharp/DXVK-MacOS) | `dxgi.dll`, `d3d11.dll`, `d3d10core.dll` | zlib (dxbc-spirv: MIT) | `LICENSE.dxvk`, `LICENSE.dxbc-spirv` |
| [MoltenVK](https://github.com/KhronosGroup/MoltenVK) and [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) | `libMoltenVK.dylib` | Apache-2.0 | `LICENSE.MoltenVK` |
| Metal Shader Converter headers | built into `libMoltenVK.dylib` | Apache-2.0 | `metal_irconverter/LICENSE.txt` in MoltenVK's source |
| [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler)'s dxilconv | `libdxbcconverter.dylib` (DXBC to DXIL) | University of Illinois/NCSA | `LICENSE.dxc.txt` |
| Apple Metal Shader Converter 4.0 beta 3 | `libmetalirconverter.dylib` | Apple's license | `LICENSE.metal-shaderconverter.rtf`, `ACKNOWLEDGEMENTS.metal-shaderconverter.rtf` |

Apple's Metal Shader Converter library is proprietary. It ships unmodified, with Apple's signature, agreement and
acknowledgements, and only for shader conversion on Macs. [docs/metal-shader-converter.md](docs/metal-shader-converter.md)
explains what it does here and how Apple's agreement allows that.

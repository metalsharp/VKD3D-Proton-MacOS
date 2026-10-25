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

<pre>D3D12 application → vkd3d-proton → Vulkan → custom MoltenVK → Metal</pre>

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
├── patches/                         # this project's changes to VKD3D-Proton, MoltenVK, SPIRV-Cross, DXVK
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
VKD3D-Proton is LGPL-2.1, DXVK is zlib, and MoltenVK and SPIRV-Cross are Apache-2.0. Each release package carries
the licenses of what it contains.

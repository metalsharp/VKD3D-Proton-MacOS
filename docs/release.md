# Releases

A release is one GitHub release, `vX.Y`, with three packages. Each package carries the licenses of what it contains
and a `SHA256SUMS` file.

```text
vkd3d-proton-vX.Y-x86_64/            for an x86_64 Wine
├── x86_64-windows/                  d3d12.dll d3d12core.dll (VKD3D-Proton), dxgi.dll d3d11.dll d3d10core.dll d3d9.dll
│                                    d3d8.dll (DXVK)
├── install-x86_64-wine.sh, install-common.sh
├── DXVK-VERSION                     the metalsharp/DXVK-MacOS tag the DXVK DLLs are built from
└── LICENSE (AGPL-3.0), LICENSE.vkd3d-proton (LGPL-2.1), LICENSE.dxvk (zlib), LICENSE.dxbc-spirv (MIT), SHA256SUMS

vkd3d-proton-vX.Y-arm64/             for an arm64 Wine that runs x86_64 programs with FEX
├── aarch64-windows/                 d3d12.dll d3d12core.dll as ARM64X (VKD3D-Proton)
├── x86_64-windows/                  dxgi.dll d3d11.dll d3d10core.dll d3d9.dll d3d8.dll as ARM64X (DXVK)
├── install-arm64-wine.sh, install-common.sh
├── DXVK-VERSION
└── LICENSE, LICENSE.vkd3d-proton, LICENSE.dxvk, LICENSE.dxbc-spirv, SHA256SUMS

moltenvk-vX.Y-universal/             for both
├── libMoltenVK.dylib                arm64 + x86_64, ad-hoc signed
├── MoltenVK_icd.json
├── libmetalirconverter.dylib        Apple's Metal Shader Converter, arm64 + x86_64, unmodified and Apple-signed
└── LICENSE.MoltenVK (Apache-2.0), LICENSE.metal-shaderconverter.rtf,
    ACKNOWLEDGEMENTS.metal-shaderconverter.rtf, SHA256SUMS
```

[install.md](install.md) covers installing them into a prefix.

## How CI builds them

`.github/workflows/release.yml` builds the three packages on `macos-26` runners from the pinned sources and this
repository's patches:

- **Sources:** `scripts/fetch-sources.sh`. DXVK comes from [metalsharp/DXVK-MacOS](https://github.com/metalsharp/DXVK-MacOS)
  at the release tag it pins (`DXVK_REV`), the same source for both lanes. Its changes for MoltenVK and MetalSharp
  Wine are made and released there, not as patches here.
- **Toolchain:** llvm-mingw, plus meson, ninja and glslang from Homebrew.
- **Build scripts:**
  - `scripts/build-vkd3d-proton.sh` for x86_64, `scripts/build-vkd3d-proton-arm64x.sh` for ARM64X;
  - `scripts/build-dxvk.sh` for DXGI, D3D10/11 and D3D8/9 as x86_64, `scripts/build-dxvk-arm64x.sh` as ARM64X;
  - `scripts/build-moltenvk.sh --externals` for MoltenVK, with the newest Xcode on the runner. It needs the macOS 26
    SDK or later.
- **Metal Shader Converter:** `scripts/fetch-metal-shaderconverter.sh` downloads Apple's `libmetalirconverter.dylib`,
  its license and acknowledgements from this repository's `deps-metal-shaderconverter-4.0b3` release, and checks
  each against the SHA-256 in the script. Apple's own download needs an Apple ID, so the repository hosts the
  unmodified files, which Apple's Metal Shader Converter license allows (section 2B: "install, use, test, and
  distribute the Dynamic Libraries for the sole purpose of shader conversion"). The dylib is never re-signed: the
  license forbids modifying it, and it already carries Apple's signature. To move to a new converter version,
  publish its files as a new `deps-metal-shaderconverter-<version>` release and update the URL and checksums in
  the script.

It runs:

- **On a tag `v*`:** builds the packages and publishes them as the release for that tag.
- **On pushes to `main`, and on pull requests that change the workflow, `scripts/` or `patches/`:** builds the
  packages as the run's artifacts, without releasing.

## Publishing

Merge to `main`, then push a tag:

```bash
git tag v2.0
git push origin v2.0
```

Keep generated packages and the local source and build trees out of commits. The releases hold the packages.

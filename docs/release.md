# Releases

A release is one GitHub release, `vX.Y`, with three packages. Each package carries the licenses of what it contains
and a `SHA256SUMS` file.

```text
vkd3d-proton-vX.Y-x86_64/            for an x86_64 Wine
├── x86_64-windows/                  d3d12.dll d3d12core.dll (VKD3D-Proton), dxgi.dll d3d11.dll d3d10core.dll (DXVK)
├── install-x86_64-wine.sh, install-common.sh
└── LICENSE (AGPL-3.0), LICENSE.vkd3d-proton (LGPL-2.1), LICENSE.dxvk (zlib), SHA256SUMS

vkd3d-proton-vX.Y-arm64/             for an arm64 Wine that runs x86_64 programs with FEX
├── aarch64-windows/                 d3d12.dll d3d12core.dll as ARM64X (VKD3D-Proton)
├── x86_64-windows/                  dxgi.dll d3d11.dll d3d10core.dll (DXVK)
├── install-arm64-wine.sh, install-common.sh
└── LICENSE, LICENSE.vkd3d-proton, LICENSE.dxvk, SHA256SUMS

moltenvk-vX.Y-universal/             for both
├── libMoltenVK.dylib                arm64 + x86_64, ad-hoc signed
├── MoltenVK_icd.json
└── LICENSE.MoltenVK (Apache-2.0), SHA256SUMS
```

[install.md](install.md) covers installing them into a prefix.

## How CI builds them

`.github/workflows/release.yml` builds the three packages on `macos-26` runners from the pinned sources and this
repository's patches:

- **Sources:** `scripts/fetch-sources.sh`.
- **Toolchain:** llvm-mingw, plus meson, ninja and glslang from Homebrew.
- **Build scripts:**
  - `scripts/build-vkd3d-proton.sh` for x86_64, `scripts/build-vkd3d-proton-arm64x.sh` for ARM64X;
  - `scripts/build-dxvk.sh` for DXGI and D3D10/11;
  - `scripts/build-moltenvk.sh --externals` for MoltenVK, with the newest Xcode on the runner. It needs the macOS 26
    SDK or later.

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

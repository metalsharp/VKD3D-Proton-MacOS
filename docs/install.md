# Installing into a Wine prefix

Each release has three packages. You need one VKD3D-Proton package, for the kind of Wine you run, and the MoltenVK
package.

| Package | For | DLLs |
|---|---|---|
| `vkd3d-proton-<tag>-x86_64.tar.zst` | an x86_64 Wine (on Intel, or under Rosetta 2) | `d3d12`, `d3d12core`, `dxgi`, `d3d11`, `d3d10core`, all x86_64 |
| `vkd3d-proton-<tag>-arm64.tar.zst` | an arm64 Wine that runs x86_64 programs with FEX (MetalSharp Wine) | `d3d12`, `d3d12core` as ARM64X; `dxgi`, `d3d11`, `d3d10core` as x86_64 |
| `moltenvk-<tag>-universal.tar.zst` | both | `libMoltenVK.dylib` (arm64 + x86_64), `MoltenVK_icd.json`, and Apple's Metal Shader Converter library `libmetalirconverter.dylib` |

The arm64 package is the VKD3D-Proton lane MetalSharp Wine runs games with. x86_64 programs load its ARM64X D3D12 DLLs
as ARM64EC code under FEX, and present with DXVK's x86_64 DXGI. It is for x86_64 programs: native arm64 programs in
the prefix can't load the x86_64 DXGI.

## Install

Extract both packages side by side and run the installer from the VKD3D-Proton package:

```bash
tar -xf vkd3d-proton-<tag>-x86_64.tar.zst
tar -xf moltenvk-<tag>-universal.tar.zst
./vkd3d-proton-<tag>-x86_64/install-x86_64-wine.sh --prefix ~/my-prefix --wine /path/to/wine
```

For an arm64 Wine, use the arm64 package and `install-arm64-wine.sh`. The scripts are also in this repository's
`install/` directory. Run from there without packages, they download the latest release, or the one `--release <tag>`
names.

| Option | Meaning |
|---|---|
| `--prefix DIR` | the Wine prefix (default: `$WINEPREFIX`, else `~/.wine`) |
| `--wine PATH` | the wine binary that runs the prefix (default: `$WINE`, else `wine` on the `PATH`) |
| `--release TAG` | the release to download |
| `--packages DIR` | packages already downloaded into DIR (`.tar.zst` or extracted) |
| `--uninstall` | put back what the install replaced, and remove its overrides and files |

The installer:

- checks the packages against their `SHA256SUMS`;
- checks that the prefix's own DLLs match the package: x86-64 for the x86_64 package, Aarch64 for the arm64 one;
- copies the DLLs into the prefix's `system32`, keeping the ones they replace in `<prefix>/vkd3d-proton-macos/backup`;
- sets `native` DLL overrides for them;
- installs MoltenVK into `<prefix>/vkd3d-proton-macos/moltenvk`, with Apple's Metal Shader Converter library beside
  it. VKD3D-Proton hands eligible DXIL compute, vertex and pixel shaders to the converter instead of compiling
  them through SPIR-V and Metal source, which cuts their compile time. `METALSHARP_MSC=0` turns this off.

## Run

Run programs with the launcher the installer writes, or source its environment first:

```bash
~/my-prefix/vkd3d-proton-macos/wine game.exe
source ~/my-prefix/vkd3d-proton-macos/env.sh && wine game.exe
```

The environment points Wine at this MoltenVK. A Wine with a Vulkan loader finds it through `VK_DRIVER_FILES`. A Wine
that loads MoltenVK as `libvulkan.dylib` finds it through `DYLD_LIBRARY_PATH`.

The environment also sets `WINEDEBUG=-all` unless `WINEDEBUG` is already set. With Wine's default debug channels on,
VKD3D-Proton reads an unfilled driver ID and refuses the device ("Lacking support for single texel alignment").

MetalSharp Wine's `bin/wine` is a wrapper that clears Vulkan's variables. Pass the binary it runs instead:
`--wine MetalSharp-Wine.app/Contents/MacOS/wine`. MetalSharp Wine also ships this lane itself:
`GRAPHICS_BACKEND=vkd3d` uses it without installing anything.

## Tested

On an Apple M4 (macOS 27.2), with the D3D12 performance probe (`tests/perf`):

- the x86_64 package, in a prefix of MetalSharp's x86_64 Wine 11.17 (under Rosetta 2): offscreen scenes and a
  swap chain (`present`);
- the arm64 package, in a prefix of MetalSharp Wine 11.20 (arm64, FEX), with the probe's x86_64 build: offscreen
  scenes.

Presenting through the arm64 package's DXGI is MetalSharp Wine's VKD3D-Proton lane. God of War Ragnarök presents
with it there.

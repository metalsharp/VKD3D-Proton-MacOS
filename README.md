# DXVK-MacOS

A maintained macOS/Wine fork of [Gcenx/DXVK-macOS](https://github.com/Gcenx/DXVK-macOS), refreshed onto upstream [DXVK v3.1.1](https://github.com/doitsujin/DXVK/releases/tag/v3.1.1).

This fork brings Gcenx's MoltenVK compatibility work to DXVK 3.1 and its `dxbc-spirv` shader compiler. It also builds ARM64X DLLs for [MetalSharp Wine](https://github.com/metalsharp/MetalSharp-Wine) (arm64 Wine with FEX).

It also carries [d7vk](https://github.com/WinterSnowfall/d7vk)'s `ddraw.dll`: Direct3D 7, 6, 5 and 3 (immediate mode) on DXVK's D3D9 backend. DirectDraw itself is proxied to Wine's own `ddraw.dll`, which d7vk loads as `ddraw_.dll` (see [Runtime setup](#runtime-setup)).

## Release contents

A `v*` tag builds a release (`.github/workflows/release.yml`) with four packages, each with its licenses and `SHA256SUMS`:

- `dxvk-macos-<tag>-x86_64`: `ddraw.dll`, `d3d8.dll`, `d3d9.dll`, `d3d10core.dll`, `d3d11.dll` and `dxgi.dll` for x86_64 Windows programs (`x86_64-windows`)
- `dxvk-macos-<tag>-i386`: the same for 32-bit Windows programs (`i386-windows`)
- `dxvk-macos-<tag>-arm64x`: the same as ARM64X for an arm64 Wine that runs x86_64 programs with FEX (MetalSharp Wine). x86_64 programs load them as ARM64EC from `x86_64-windows`, so DXVK itself runs natively.
- `moltenvk-<tag>-universal`: the universal (arm64 + x86_64), ad-hoc signed `libMoltenVK.dylib` and `MoltenVK_icd.json`. It is built by [metalsharp/VKD3D-Proton-MacOS](https://github.com/metalsharp/VKD3D-Proton-MacOS)'s `scripts/fetch-sources.sh moltenvk` and `scripts/build-moltenvk.sh --externals` at the commit the workflow pins (`VKD3D_PROTON_MACOS_REV`). That is the same MoltenVK, with the same MoltenVK and SPIRV-Cross fixes, that VKD3D-Proton-MacOS ships and MetalSharp Wine runs. DXVK 3.1 needs those fixes on Metal:
  - workgroup sizes taken from `LocalSizeId`;
  - runtime-sized descriptor arrays;
  - compute shaders with groupshared memory;
  - geometry pipelines without multisample state;
  - geometry shader limits.

Pushes and pull requests build the x86_64, i386 and ARM64X lanes in `.github/workflows/build-macos-cross.yml`. The ARM64X DLLs are checked with `tools/ci/check-arm64x.py`.

The DXVK `dxgi.dll` is a D3D8/9/10/11 lane. VKD3D-Proton's D3D12 lane uses DXVK's `dxgi.dll`, `d3d11.dll` and `d3d10core.dll` for swap chains and D3D11On12. VKD3D-Proton-MacOS's arm64 release pins this fork's ARM64X build for that. Don't mix in a `dxgi.dll` from another DXVK version.

## Requirements

- Meson and Ninja
- `glslangValidator`
- Git with recursive submodule support
- For the x86_64 and i386 lanes, MinGW-w64 cross-compilers: `x86_64-w64-mingw32-gcc/g++` and `i686-w64-mingw32-gcc/g++`
- For the ARM64X lane, [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (20261020 or later), with the `arm64ec-w64-mingw32` target, on `PATH`
- At run time: MoltenVK, from the release's `moltenvk` package or a VKD3D-Proton-MacOS build. Upstream MoltenVK lacks the fixes above, and DXVK 3.1 renders parts of games wrong or not at all with it.

DXVK 3.1 needs Vulkan 1.3, `VK_KHR_maintenance5`, `VK_KHR_maintenance6` and `VK_KHR_load_store_op_none`. The MoltenVK above reports them on Apple silicon.

## Build from source

```bash
git clone --recursive https://github.com/metalsharp/DXVK-MacOS.git
cd DXVK-MacOS
```

Apply the MoltenVK patch to the `dxbc-spirv` submodule before building:

```bash
git submodule update --init --recursive
git -C subprojects/dxbc-spirv apply ../../patches/dxbc-spirv-moltenvk.patch
```

Build the 64-bit Windows lane:

```bash
meson setup build.x86_64 \
  --cross-file build-win64.txt \
  --buildtype release \
  --prefix "$PWD/stage/x86_64"
meson compile -C build.x86_64
meson install -C build.x86_64
```

Build the 32-bit Windows lane:

```bash
meson setup build.i386 \
  --cross-file build-win32.txt \
  --buildtype release \
  --prefix "$PWD/stage/i386"
meson compile -C build.i386
meson install -C build.i386
```

Build the ARM64X lane for MetalSharp Wine, with llvm-mingw on `PATH`:

```bash
meson setup build.arm64x \
  --cross-file build-arm64x.txt \
  --buildtype release \
  --prefix "$PWD/stage/arm64x"
meson compile -C build.arm64x
meson install -C build.arm64x
python3 tools/ci/check-arm64x.py stage/arm64x/*/*.dll
```

`build-arm64x.txt` builds with `-marm64x`. Each DLL then holds ARM64 code for arm64 programs and ARM64EC code for x86_64 programs.

## MoltenVK options

These `dxvk.conf` options exist only in this fork:

```ini
# Each D3D9 sampler slot gets a separate binding per texture type (2D, cube, 3D). Metal cannot
# alias image views of different types on one binding. Auto: on with MoltenVK.
d3d9.deAliasedSamplers = Auto

# D3D9 draws that sample a bound render target read a copy of it instead. Metal has no
# feedback loops. Auto: one copy per draw with MoltenVK. False: one copy shared between draws.
d3d9.renderTargetHazardCopies = Auto
```

The second option ports [Gcenx/DXVK-macOS#21](https://github.com/Gcenx/DXVK-macOS/pull/21) to DXVK 3.1.

On MetalSharp Wine, DXVK adds a line to the Metal performance HUD with the API, its feature level and DXVK's version. For example, `DXVK D3D11 FL_11_1 <version>`.

## Wine Vulkan portability fix

MoltenVK is reported as a Vulkan portability driver. Wine must enable `VK_KHR_portability_enumeration` and `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` when creating the host Vulkan instance.

Apply the included patch to the matching Wine source tree:

```bash
cd /path/to/wine-source
git apply /path/to/DXVK-MacOS/patches/wine-vulkan-portability.patch
```

Rebuild the Unix `win32u` library and use the rebuilt Wine runtime. Keep the original Wine runtime backed up until application validation is complete.

## Runtime setup

Use a wow64 prefix for Steam-style mixed 64-bit/32-bit applications:

```bash
export WINEARCH=wow64
export WINEPREFIX=/path/to/prefix
```

Keep the DXVK DLLs and MoltenVK files in the same graphics lane. Set the loader and ICD paths before launching:

```bash
export DXVK_CONFIG_FILE=/path/to/dxvk.conf
export VK_ICD_FILENAMES=/path/to/moltenvk/MoltenVK_icd.json
export DYLD_LIBRARY_PATH=/path/to/moltenvk
export WINEDLLOVERRIDES='d3d8,d3d9,d3d10core,d3d11,dxgi=n,b'
```

Copy the DLLs into the prefix's `system32` (64-bit, or ARM64X on MetalSharp Wine) and `syswow64` (i386) folders, or into the game's folder.

### Direct3D 7, 6, 5 and 3 (`ddraw.dll`)

`ddraw.dll` implements only Direct3D and passes DirectDraw itself to a real DirectDraw. It loads `ddraw_.dll` from the
game's folder first, then from the system folder. Copy Wine's own `ddraw.dll` of the same architecture
(`lib/wine/i386-windows/ddraw.dll` for 32-bit games, which nearly all of them are) next to it as `ddraw_.dll`, and set
`ddraw=n,b`:

```bash
export WINEDLLOVERRIDES='ddraw=n,b'
```

Some games (GTA 2, StarLancer, Midtown Madness 2) only load DirectDraw from the system folder; install both files in
`syswow64` for them. D3D9's `d3d9.*` options apply to these games too, and `ddraw.*` options are listed in `dxvk.conf`.
The log is `<game>_ddraw.log`. `tools/smoke/ddraw_run.sh` runs the D3D7/6/5/3 checks in `tools/smoke/ddraw_draw.c`.

## Provenance

- Upstream DXVK: `v3.1.1` (`1e0f92318d6ec7b7286e050d9e24fd01a2d09c92`), merged up to master `0f5c295c66770895a68542ea9cee2c77ac7dfd9f` through d7vk
- d7vk (`src/ddraw` and its D3D9 changes): `devel` at `9fb6a2d50`
- Upstream `dxbc-spirv`: the submodule's pinned commit (`git ls-tree HEAD subprojects/dxbc-spirv`)
- MoltenVK and SPIRV-Cross: [metalsharp/VKD3D-Proton-MacOS](https://github.com/metalsharp/VKD3D-Proton-MacOS) (`patches/moltenvk`, `patches/spirv-cross`)
- Gcenx DXVK-macOS heritage: `1.10.x` branch and release history
- Ported PR #20 commits are recorded in `patches/dxbc-spirv-moltenvk.patch` and the release README

This project is an independent maintained fork and is not an official release of Gcenx, doitsujin, Wine, or MoltenVK. Preserve upstream copyright and license notices when redistributing binaries.

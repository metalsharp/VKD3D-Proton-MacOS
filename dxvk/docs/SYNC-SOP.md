# Upstream Sync SOP (P0.2)

Cadence: **on every upstream release tag** + a weekly `master` sweep. Never let
`git rev-list --count HEAD..upstream/master` exceed one release cycle (~60 commits).

## Remotes
- `upstream`: https://github.com/doitsujin/dxvk.git
- `d7vk`: https://github.com/WinterSnowfall/d7vk.git. Its default branch is `devel` (DXVK plus `src/ddraw` and
  its D3D9 hooks); its `master` only mirrors upstream. d7vk merges upstream within days, so merging `d7vk/devel`
  is also an upstream sync. Merge `upstream/master` directly only when d7vk lags behind it.

## Procedure
1. `git fetch upstream --tags && git fetch d7vk && git checkout -b sync/<upstream-tag> main`
2. `git merge d7vk/devel` (or `upstream/master`, see Remotes) — resolve conflicts favoring upstream except in:
   - `src/dxvk/dxvk_device_info.cpp` (MoltenVK feature table)
   - `src/d3d9/*` (deAliasedSamplers & fixed-function shims)
   - `patches/*`, `.github/workflows/*`
   - Things d7vk changes that this fork keeps as DXVK's: `src/d3d9/meson.build` (both `d3d9.dll` and the
     `d3d9_static` library that `ddraw.dll` links; the D3D9 logger lives in `d3d9_logger.cpp`, not
     `d3d9_main.cpp`), `DXVK_LOG_PATH`/`DXVK_LOG_LEVEL` in `src/util/log/log.cpp`, the "DXVK" names in
     `dxvk_instance.cpp` and the HUD, the shader cache in `dxvk_device.cpp`, and `package-release.sh`, the
     issue templates and `test-build-windows.yml`
   - Submodules: take upstream's pointers (d7vk's are upstream's)
3. **Re-validate `patches/dxbc-spirv-moltenvk.patch`** whenever the submodule pointer moved:
   ```bash
   git submodule update --init --recursive subprojects/dxbc-spirv
   git -C subprojects/dxbc-spirv apply --check ../patches/dxbc-spirv-moltenvk.patch
   ```
   If it fails: re-derive the patch from upstream's diff of the failing hunk.
4. Re-run the capability probe; diff against `docs/baselines/*.json` — any feature
   change gets a gap-policy row update in the same commit.
5. Cross-build the i386, x86_64 and ARM64X lanes (CI or local), run `tools/smoke.sh d3d9`, and
   `tools/smoke/ddraw_run.sh` on both MetalSharp Wines (i386 and 64-bit programs).
6. Merge to `main`, tag `<upstream-tag>-macos.N`, write release notes.

## Conflict policy
Upstream owns everything except the documented MoltenVK shim set. If upstream
fixes something a shim worked around, **delete the shim** and record it in
`docs/gap-policy.md` (shims are liabilities, not assets).

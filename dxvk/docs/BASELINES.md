# Baselines (P0.1)

Pinned references for all modernization work. Update only via a sync commit.

## Upstream (doitsujin/DXVK)
- **Re-sync target at audit time:** `master @ 40e01640` — "[dxvk] End render pass before recording frame capture markers" (2026-09-19)
- **Last release tag:** `v3.1.1` (2026-09-15)
- **Fork base:** `v3.1` tag commit `70d7508c` (2026-08-27)
- **Delta to fold in (Phase 1):** 44 commits (`git rev-list --count HEAD..upstream/master`)

## This fork (metalsharp/DXVK-MacOS)
- Pre-sync tip: `29e0f596` — "Update README for clarity on DXVK 3.1 changes" (2026-09-12)
- Fork-specific patch set (5 commits): `849f05ac` (D3D9 MoltenVK port), `d99e447b`+`214f092a` (CI), `bd8921a3` (workflow cleanup), `29e0f596` (README)

## Submodules / patches
- `subprojects/dxbc-spirv`: upstream bumped it **2×** inside the 44-commit delta — the maintained `patches/dxbc-spirv-moltenvk.patch` **must** be re-validated (P1.3/P1.4)
- `patches/wine-vulkan-portability.patch`: applies to Wine `win32u`; validated against **MetalSharp Wine 11.17** runtime

## Validation environment (this lab)
- Apple M4, macOS 27.2 (26B5086k), Rosetta 2
- Wine: **MetalSharp wine-11.17** runtime bundle (x86_64, wow64: i386+x86_64 PE), bundled MoltenVK `1.4.3` (Vulkan `1.4.357`) + `libvulkan.dylib` + ICD
- Native probe evidence: `docs/baselines/baseline-M4-macOS27.json` (P0.10)

# Lab Matrix (P0.6)

Hardware/runtime combos used for validation. Every Phase 2–4 acceptance item
must state which matrix cells it ran on.

| Cell | Hardware | macOS | Wine runtime | MoltenVK | Status |
|---|---|---|---|---|---|
| M4-LAB | Apple M4 (arm64, Rosetta for Wine) | 27.2 (26B5086k) | **MetalSharp wine-11.17** runtime bundle (`metalsharp-wine`, x86_64, wow64 i386+x86_64) | bundled `1.4.3` (Vulkan 1.4.357) | **active** — probe baseline recorded |
| OTHER | *add Intel/AMD Mac rows here* | — | — | — | open |

## Notes
- Wine runs under Rosetta 2; PE i386 tests go through wine's wow64 path.
- The MetalSharp runtime bundles its own `libMoltenVK.dylib` + `MoltenVK_icd.json`
  (in `runtime/wine/lib/moltenvk-vkmt/`) and ICD config (`runtime/wine/etc/vulkan/icd.d/`).
- Native (non-Wine) probe runs use the same universal dylib's arm64 slice.
- **Only MetalSharp wine-11.17 is used for Wine validation** (project rule).

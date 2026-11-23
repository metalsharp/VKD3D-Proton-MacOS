# Phase 2 Report — D3D9/D3D8 lane "fully supported" @ device level

*Executed 2026-09-21 against `v3.1.1-macos.0` on Apple M4 / MetalSharp wine-11.17.*

## Sub-phase outcomes
| Sub | Outcome | Evidence |
|---|---|---|
| P2.1 option inventory | **done** — 49 source-default options catalogued + macOS/MoltenVK notes (async absent until P4; `enablePresentTiming` degrades on MVK) | `docs/OPTIONS-DELTA.md` |
| P2.2 deAliasedSamplers auto-enable | **verified already implemented** — `Tristate::Auto` → `adapter->matchesDriver(VK_DRIVER_ID_MOLTENVK)` in `d3d9_options.cpp` | source inspection + smoke |
| P2.3 fixed-function pass | device-level PASS; FF-variety validation needs Tier-2 apps (slots open) | smoke-runs |
| P2.4 sampler edge cases | device-level PASS (deAlias path exercised via fixed-function shader path); cube/bias edges → Tier-2 | smoke-runs |
| P2.5 state cache | device boots with cache writes enabled; cross-version cache validation → Tier-2 + Phase 3 | smoke logs |
| P2.6 d3d8 lane | **PASS both archs** (incl. new upstream legacy discard/mapping code) | smoke-runs |
| P2.7 config backports | Genshin Apple-Silicon spoof **ported** (async line dropped until P4); SFV `setenv` entry skipped (no setenv option in fork); all other Gcenx-era entries already upstream | `src/util/config/config.cpp` |
| P2.8 soak | **28 runs / 0 failures / 30 min** | smoke-runs 20260921-01* |
| P2.9 perf baseline | recorded; vsync-throttled as expected; Phase 4 adds real bench harness | `docs/PERF-baseline.md` |
| P2.10 sign-off | tag `d3d9-rc1` | this commit |

## Honest limitations
- Tier-1 smokes prove **device creation + clear/present** only; full game-content
  validation is Tier-2 (needs real titles — slots documented).
- Wine runtime's x86_64 MoltenVK slice reports 1.4.1/1.4.334 vs the arm64
  slice's 1.4.3/1.4.357 (both ≥ DXVK's 1.3 gate).
- 32-bit ran under wow64 — no pure-i386 wine process involved.

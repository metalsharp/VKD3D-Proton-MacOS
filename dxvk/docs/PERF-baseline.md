# Perf Baseline (P2.9)

*Apple M4 · MetalSharp wine-11.17 (Rosetta) · MoltenVK bundled · DXVK v3.1.1-macos.0 · windowed clear/present, vsync interval default · generated 2026-09-21*

| Run | Lane | Frames | Measured FPS |
|---|---|---|---|
| `d3d11_smoke-20260921-003057` | d3d11 | 600 | 59.7 |
| `d3d11_smoke32-20260921-003110` | d3d11 | 600 | 62.7 |
| `d3d8_smoke-20260921-003120` | d3d8 | 600 | 176.9 |
| `d3d8_smoke32-20260921-003124` | d3d8 | 600 | 101.0 |
| `d3d9_smoke-20260921-003131` | d3d9 | 600 | 62.8 |
| `d3d9_smoke32-20260921-003141` | d3d9 | 600 | 101.7 |

**Method:** `tools/smoke.sh run all 600` — `Present()` with sync interval
(vsync-capped); MoltenVK pipeline-compile warm-up happens inside each run's
first frames and is not excluded from wall time.

**Reading:** all three lanes present stably on both PE architectures; fps is
swapchain-throttled as expected at these frame counts. Absolute throughput
benchmarking (uncapped, 1% lows, compile-storm timing) is deferred to Phase 4's
harness (P4.7). These numbers are the **regression baseline** for Phases 2–4.

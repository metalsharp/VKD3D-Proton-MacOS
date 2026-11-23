# Test Applications (P0.7)

Canonical smoke set. Tier 1 apps are built in-repo (license-free, deterministic);
Tier 2 slots are for real games/manual QA (add rows as they are acquired).

## Tier 1 — built by `tools/smoke.sh` (automated)
| App | Lane | What it proves |
|---|---|---|
| `d3d9_smoke` | d3d9 | D3D9Ex device creation, clear, EndScene/Present × N, FPS counter |
| `d3d8_smoke` | d3d8 | D3D8 device creation + clear + present (rides d3d9 lane) |
| `d3d11_smoke` | d3d11 + dxgi | D3D11 device + DXGI swapchain creation, clear, Present × N |

## Tier 2 — manual (fill in)
| App | Lane | Exercises | Verdict | Evidence |
|---|---|---|---|---|
| *(slot)* fixed-function era game | d3d9 | FF lighting/fog/texgen, deAliasedSamplers | — | — |
| *(slot)* FL10 raster-heavy game | d3d11 | geometry pipelines, no GS | — | — |
| *(slot)* FL11 tessellation title | d3d11 | hull/domain (MVK compute pre-pass) | — | — |
| *(slot)* tiled-resources title | d3d11 | sparse residency (probe says supported!) | — | — |
| *(slot)* stream-out title | d3d9/11 | SO paths → expect graceful-fail | — | — |

## Acceptance thresholds (Tier 1)
- Device creation succeeds; zero `ERR` lines in DXVK logs
- ≥100 frames presented without device-loss; exit code 0
- 30-min soak variant (`--soak`) for Phase 2 sign-off

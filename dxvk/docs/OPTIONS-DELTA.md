# Options Delta — DXVK 3.1.1-sync option inventory (P2.1)

*49 options found in source defaults; macOS/MoltenVK-relevant rows annotated.*

| Option | Default | macOS bundle note |
|---|---|---|
| `d3d11.cachedDynamicResources` | `std::string(` |  |
| `d3d11.clampNegativeLodBias` | `false` |  |
| `d3d11.disableDirectImageMapping` | `false` |  |
| `d3d11.disableMsaa` | `false` |  |
| `d3d11.enableContextLock` | `false` |  |
| `d3d11.exposeDriverCommandLists` | `true` |  |
| `d3d11.forceComputeLdsBarriers` | `false` |  |
| `d3d11.forceComputeUavBarriers` | `false` |  |
| `d3d11.forceSampleRateShading` | `false` |  |
| `d3d11.maxTessFactor` | `0` | MVK tessellation via compute pre-pass; cap if perf issues |
| `d3d11.relaxedBarriers` | `false` |  |
| `d3d11.relaxedGraphicsBarriers` | `false` |  |
| `d3d11.reproducibleCommandStream` | `false` |  |
| `d3d11.samplerAnisotropy` | `-1` |  |
| `d3d11.samplerLodBias` | `0.0f` |  |
| `d3d8.forceVsDecl` | `""` |  |
| `d3d8.scaleDref` | `0` |  |
| `d3d9.floatEmulation` | `"auto"` |  |
| `d3d9.useFP16` | `Tristate::False` |  |
| `dxgi.customDeviceDesc` | `""` |  |
| `dxgi.deferSurfaceCreation` | `false` |  |
| `dxgi.enableDummyCompositionSwapchain` | `false` |  |
| `dxgi.enableHDR` | `env::getEnvVar("DXVK_HDR"` |  |
| `dxgi.enableUe4Workarounds` | `false` |  |
| `dxgi.forceRefreshRate` | `0u` |  |
| `dxgi.hideAmdGpu` | `Tristate::Auto` |  |
| `dxgi.hideIntelGpu` | `Tristate::Auto` |  |
| `dxgi.hideNvidiaGpu` | `Tristate::Auto` |  |
| `dxgi.hideNvkGpu` | `Tristate::Auto` |  |
| `dxgi.maxDeviceMemory` | `0` |  |
| `dxgi.maxFrameLatency` | `0` |  |
| `dxgi.maxSharedMemory` | `0` |  |
| `dxgi.syncInterval` | `-1` |  |
| `dxvk.deviceFilter` | `""` |  |
| `dxvk.disableNvLowLatency2` | `Tristate::Auto` |  |
| `dxvk.enableDescriptorBuffer` | `Tristate::Auto` |  |
| `dxvk.enableDescriptorHeap` | `Tristate::Auto` |  |
| `dxvk.enableDescriptorUpdateTemplates` | `false` |  |
| `dxvk.enableGraphicsPipelineLibrary` | `Tristate::Auto` | no-op on MoltenVK (ext absent per probe) |
| `dxvk.enableMemoryDefrag` | `Tristate::Auto` |  |
| `dxvk.hud` | `""` | works; compiler pipeline line useful given no GPL |
| `dxvk.latencySleep` | `Tristate::Auto` |  |
| `dxvk.lowerSinCos` | `Tristate::Auto` |  |
| `dxvk.maxFrameRate` | `config.getOption<int32_t>("dxgi.maxFrameRate", 0` |  |
| `dxvk.maxMemoryBudget` | `0` |  |
| `dxvk.tearFree` | `Tristate::Auto` | syncs with Mac display; ok |
| `dxvk.tilerMode` | `Tristate::Auto` |  |
| `dxvk.trackPipelineLifetime` | `Tristate::Auto` |  |
| `dxvk.useRawSsbo` | `Tristate::Auto` |  |

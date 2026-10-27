# Tracing a problem: `METALSHARP_TRACE`

One environment variable turns on the logging a bug needs, in VKD3D-Proton and MoltenVK at once:

```bash
METALSHARP_TRACE=present,sync wine game.exe
```

`METALSHARP_TRACE` takes a comma-separated list of categories. A category followed by `+heavy`
(`gpu+heavy`) adds that category's slow modes. A bare `heavy` makes every listed category heavy, so
`all,heavy` (or `all+heavy`) turns everything on.

## Categories

| Category | Use it for | VKD3D-Proton | MoltenVK | `+heavy` adds |
|---|---|---|---|---|
| `shader` | shaders that fail, render wrong, or stutter while compiling | shader dumps to `shaders/`, pipeline library hits, compile time per pipeline state | SPIR-V and MSL dumps to `shaders/`, Metal compile time per pipeline and library | |
| `present` | stutter, frame pacing, black or frozen frames | one line per Present: frame, flags, time since the last Present, time in Present; latency markers | drawable wait per image, present scheduling, time from commit to on-screen and between frames | |
| `memory` | out-of-memory, leaks, budget problems | memory budget, every device memory allocation and free | every `vkAllocateMemory`/`vkFreeMemory` and MTLHeap, texture debug output | |
| `resource` | buffers and textures created wrong or failing | committed, placed and reserved resources, heaps | every buffer and image created and destroyed | GPU descriptor validation (`descriptor_qa_checks`) |
| `sync` | hangs, deadlocks, CPU waits on the GPU | queue Signal/Wait, ExecuteCommandLists, fence waits and CPU signals | host waits on fences and timeline semaphores of 1 ms or more, and every timeout | every wait and every host signal |
| `gpu` | device lost, GPU hangs, GPU errors | device fault reports | hang watchdog: a command buffer committed for 2 s is reported as waiting to be scheduled or running; command buffer errors | GPU time of every command buffer; Metal debug mode |
| `cpu` | low frame rate, CPU-bound frames | queue timeline as Chrome trace JSON (`queue-profile.json`) | MoltenVK performance statistics every 600 frames | extra timeline events |
| `api` | finding the Vulkan call that misbehaves | | every Vulkan call with its duration | the calling thread's id |
| `pass` | wrong render pass loads, stores, or attachments | | render pass and attachment debug output | |
| `all` | everything above | | | |

## Where the logs go

Each game gets its own folder: `~/.metalsharp/logs/trace/<exe name>/`, or
`$METALSHARP_HOME/logs/trace/<exe name>/` when `METALSHARP_HOME` is set.
`METALSHARP_TRACE_DIR` overrides the folder.

| File | Written by |
|---|---|
| `vkd3d.log` | VKD3D-Proton (trace lines are tagged `trace.<category>`) |
| `moltenvk.log` | MoltenVK (lines are `<ms since start> [<category>] <thread>: message`) |
| `shaders/` | both layers, with `shader` |
| `queue-profile.json` | VKD3D-Proton, with `cpu`; open it in `chrome://tracing` or Perfetto |

Each launch moves the previous launch's logs to `vkd3d.prev.log` and `moltenvk.prev.log`.

## Other settings

- `METALSHARP_TRACE_GPU_HANG_MS` sets how long a command buffer may stay committed before the watchdog
  reports it. The default is 2000.
- A setting already in the environment wins. The flag never replaces `VKD3D_SHADER_DUMP_PATH`,
  `VKD3D_LOG_FILE`, `VKD3D_QUEUE_PROFILE` or an `MVK_CONFIG_*` value you set yourself, and it appends its
  flags to `VKD3D_CONFIG` instead of replacing it.
- Shader dumps cover only shaders compiled in this launch. Shaders already in a pipeline cache are not
  converted again, so set `MVK_CONFIG_PIPELINE_CACHE_PATH=0` (and clear `vkd3d-proton.cache`) for a full capture.
- `gpu` does not turn on VKD3D-Proton's breadcrumbs. They need `VK_AMD_buffer_marker` or
  `VK_NV_device_diagnostic_checkpoints`, which MoltenVK lacks, and with them on the D3D12 device fails to create.

## Cost

Without `METALSHARP_TRACE`, each trace point is a single check of a cached bit mask. `present`, `sync`,
`memory` and `gpu` are light enough to play with. `shader` slows the first compile of each pipeline while
it writes dumps. `api`, `resource` in busy games, and every `+heavy` mode slow the game noticeably and
write large logs, so turn them on only around the problem.

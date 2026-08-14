---
id: 110
title: Windowed startup crashes while rebuilding the resolve pipeline
status: resolved
symptom: ./run.sh with a window deterministically SIGSEGVs at frame 6 in spv::Builder::createAccessChain/createBinOp while BuildResolveComputeShader rebuilds
tags: render,present,vulkan,lifetime,interactive
created: 2026-08-14
updated: 2026-08-14
---

## Root cause

The first rendered startup frame needed a 1280x1440 EDRAM sample grid. The next accepted frame needed only 1280x720. `Renderer::RenderFrameImpl` treated any extent change as a new exact size and called `ReleasePersistent()`. In a windowed run the presenter shares the Vulkan device and may still be blitting `presentStage` after the renderer fence has signalled, so the shrink destroyed an image still in use. The resulting heap corruption surfaced during the next SPIR-V builder allocation; the shader builder was the victim, not the cause. An unconditional diagnostic printed `persistent renderer extent changed: 1280x1440 -> 1280x720` immediately before the reproducible fault.

## Fix and evidence

Persistent EDRAM dimensions are now capacity, not exact current-frame dimensions. A smaller sample grid reuses the allocation. Growth waits for all shared-device work before releasing resources, and every newly discovered surface is allocated at retained capacity. The policy lives in `gpu_extent_capacity.h`; cross-thread resource lifetime lives in `gpu_renderer_capacity.cpp`; the 3,900-line orchestrator shrank rather than growing.

Before: two current builds crashed at frame 6 with the same stack. After: a normal Wayland window survived through frame 667 and a controlled X11 run survived through frame 10,109 before exact-PID shutdown. A virtual Xbox controller then drove the live title from Press Start to Main Menu, Campaign, and Single Player while the runtime logged every press and release packet edge. `test_gpu_extent_capacity` covers shrink, growth, equality and one-axis growth.

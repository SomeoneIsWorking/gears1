---
id: 127
title: Renderer hot path treated Lucent's cached configuration lookup as free
status: resolved
symptom: captured gameplay frame spends about five avoidable milliseconds in flat per-draw and per-texture CPU work
tags: performance,gpu,draw-loop,config,mutex,fixed
created: 2026-08-22
updated: 2026-08-22
---

Root cause: Lucent caches environment values, but every config read still takes the global config mutex, constructs the prefixed key, and searches its maps. RenderFrameImpl called DRAW_NODEPTHBIAS once per issued draw, DeriveSystemConstants called DRAW_NO_TEX_SIGNS once per uniform rebuild, and TextureBinder::SelectView reparsed DRAW_TEX_BINDS once per resolved texture binding. These values are invariant while a frame is recorded.\n\nFix: runtime/gpu_draw_options owns one FrameOptions snapshot per RenderFrame call and passes the typed values into the draw, uniform, and texture owners. It is per-frame rather than process-static so frame_replay can still change a control arm between renders in one process.\n\nEvidence: a temporary interleaved A/B on scratch/frames/chapter45_recovered.gfr alternated the old repeated lookup path and the new snapshot over byte-identical input. After 12 warm-up renders, old-path draw loops averaged 39.39 ms over 44 frames and snapshot draw loops averaged 34.25 ms over 43 frames: -5.13 ms, larger than the run's 2.30 ms resolution threshold. The temporary old arm was removed after measurement. scratch/logs/hot_config_ab.log is untracked run evidence.\n\nFalsifier: the optimization is invalid if a renderer control is allowed to change during one RenderFrame call, or if a paired same-process benchmark no longer resolves a reduction on a representative draw-heavy capture. tests/test_gpu_draw_options.cpp proves consecutive frame snapshots observe an explicit Lucent cache refresh while retaining default and control semantics.

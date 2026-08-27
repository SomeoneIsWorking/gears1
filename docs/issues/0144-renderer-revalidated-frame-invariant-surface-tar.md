---
id: 144
title: Renderer revalidated frame-invariant surface targets for every draw
status: resolved
symptom: The chapter-45 draw loop repeatedly copied surface-format sets and reran target-capacity policy for every draw after the full frame plan was already known
tags: performance,renderer,vulkan,render-target-cache
created: 2026-08-27
updated: 2026-08-27
---

Root cause: PlanResolves completes RenderTargetCache::formatsPerBase before draw preparation, but every later GetSurfaceTarget call copied that base's format set, merged persistent capacity, reselected the host format, and reread control configuration. The decision is invariant for a (base, sample model) during that RenderTargetCache lifetime.

Resolution: RenderTargetCache now owns a bounded per-frame lookup keyed by the 12-bit EDRAM base and native-multisample model. The first lookup retains the full promotion/capacity path; later preparation and command-recording lookups reuse the verified SurfaceTarget pointer. GEARS_DRAW_NO_TARGET_LOOKUP_CACHE restores the old path and GEARS_DRAW_AB_TARGET_LOOKUP alternates it in one process.

Evidence: chapter45_recovered, 201 renders: cached experimental arm 20.42 ms vs repeated-validation baseline 24.63 ms over 94/94 silent frames, -4.21 ms against a 1.05 ms resolution floor. After the renderer-specific timing policy moved from the oversized frame function into gpu_draw_ab.*, a second 201-render run reconfirmed the saving at -6.27 ms (39.91 vs 46.17 ms, 3.58 ms floor). Fresh-process final framebuffers are byte-identical, SHA-256 3b34082ab05198fa4733a50c7fe6e671b32c3871be38f7b563154ec741f80c25. test_gpu_surface_target_lookup proves sample-model separation and invalid-base refusal.

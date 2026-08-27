---
id: 142
title: Renderer rebuilt report-only map censuses on every silent frame
status: resolved
symptom: The renderer performed std::map/std::set census updates for every draw even when the frame emitted no report
tags: performance,gpu,draw-loop,diagnostics,fixed
created: 2026-08-27
updated: 2026-08-27
---

## Root cause

`FrameCensus::NoteDraw`, `NoteDepth`, and `Skip` populated map/set-backed report facts unconditionally. Silent frames discarded those structures at frame end. The viewport-string census had already been gated, but the sibling censuses were missed.

## Resolution

`FrameCensus` now has an explicit collection boundary. `RenderFrameImpl` enables it only for report frames; `GEARS_DRAW_AB_CENSUS=1` restores all map/set-backed census work on alternate silent frames for measurement. `test_gpu_draw_census` proves disabled collection produces no facts while report collection remains complete.

On `chapter45_recovered.gfr`, the 201-frame interleaved run resolved census-on at +1.23 ms draw-loop time: 31.77 versus 30.54 ms over 94/94 frames, above the 1.05 ms resolution floor.

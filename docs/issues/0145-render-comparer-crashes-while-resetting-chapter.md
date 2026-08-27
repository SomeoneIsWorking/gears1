---
id: 145
title: Render comparer crashes while resetting chapter-45 second arm
status: open
symptom: GEARS_DRAW_AB=DRAW_NO_TARGET_LOOKUP_CACHE frame_replay chapter45_recovered.gfr exits 139 during the second arm before comparison
tags: harness,renderer,frame-replay,crash
created: 2026-08-27
updated: 2026-08-27
---

The first arm writes all 1,555 comparison rows. After ResetRendererForComparison, the second arm recreates pipelines and targets, then segfaults during draw preparation before writing its comparison stream. Both implementations render successfully in independent headless processes and produce the same framebuffer hash, so this is isolated to the two-render reset/comparer path rather than the surface lookup behavior. Root cause is not established yet; do not call this resolved from the fresh-process hash.

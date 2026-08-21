---
id: C076
kind: claim
status: holds
created: 2026-08-21
tags: render,msaa,gameplay
depends: runtime/gpu_draw_sample_layout.h, runtime/gpu_draw_resolve.cpp, runtime/gpu_draw_targets.cpp
reconfirmed: 2026-08-21
verified_at: 2026-08-21 12:25:41
---

## Claim

Native Vulkan 2X attachments reproduce the missing in-game wall-light coverage and materially converge the gameplay resolves on Xenia.

## Evidence

walk_gameplay.gfr: draw 650 is 79,253 fragments on both renderers; all 16 resolve passes pair with no coarse mismatch; initial scene-colour MAE improves 0.000990 to 0.000072 and later scene f32 MAE 0.001505 to 0.000000823; GEARS_DRAW_VALIDATE reports no error.

## What would falsify it

A replay with the native 2X path fails to reproduce draw 650, a paired resolve regresses beyond the expanded-grid baseline, or Vulkan validation reports the path invalid.

## Re-confirmed 2026-08-21

Final persistent three-repeat headless replay is Vulkan-validation clean and its last frame keeps draw 650 at 79,253; the earlier 16-pass layer comparison and MAE measurements remain the final pixel evidence.

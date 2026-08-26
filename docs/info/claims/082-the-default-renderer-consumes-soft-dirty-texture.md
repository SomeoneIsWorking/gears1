---
id: C082
kind: claim
status: holds
created: 2026-08-26
tags:
depends: runtime/gpu_draw_textures.cpp#TextureUploader::BeginStalenessFrame, runtime/gpu_draw_textures.cpp#TextureUploader::EndStalenessFrame, runtime/guest_dirty_pages.cpp#GuestDirtyPages::BeginObservationPeriod, runtime/gpu_draw_options.cpp#ReadFrameOptions
---

## Claim

The default renderer consumes soft-dirty texture observations before clearing for the next frame, preserving inter-frame writes while removing a resolved 3.99 ms from the chapter-45 draw loop

## Evidence

Kernel-backed guest_dirty_pages regression writes between simulated frames and is detected before clear; post-fix interleaved chapter45_recovered A/B measured 33.20 ms page-skip versus 37.19 ms full-hash over 94/94 frames with a 3.31 ms resolution floor; default replay skipped 244 textures/49.39 MiB and exactly matched the full-hash image at SHA-256 3b34082ab05198fa4733a50c7fe6e671b32c3871be38f7b563154ec741f80c25; rebuilt headless live boot armed four aliases and evicted one changed cached startup texture with zero misses. Evidence: scratch/logs/texdirty_ordered_default_ab_20260826.log, texdirty_ordered_default_report_20260826.log, texdirty_postfix_default_hash_20260826.log, texdirty_postfix_control_hash_20260826.log, texdirty_ordered_default_live_current_20260826.log

## What would falsify it

a guest write after one frame's texture checks is skipped as clean on the next frame, any forced verification reports a contradiction, default output differs from the full-hash control, or a representative paired replay no longer resolves the default path faster

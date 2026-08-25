---
id: 137
title: Renderer texture staleness hashing consumes a double-digit share of CPU cycles
status: resolved
symptom: A 101-frame chapter45_recovered replay spends 5-10 ms per steady frame rereading about 49.39 MiB of guest texture storage; perf attributes 12.54% of sampled CPU cycles to SSE2 XXH3 streaming updates
tags: performance,gpu,draw,textures,hashing
created: 2026-08-25
updated: 2026-08-25
---

## Root cause


## What was tried / dead ends


## Resolution

### Dead end (2026-08-25)
Exact within-frame storage deduplication was instrumented on chapter45_recovered.gfr over 101 replays: all 244 checked texture identities mapped to 244 unique address-and-length span pairs, so it avoided 0.00 MiB. The implementation was removed rather than retaining useless lookup overhead. Evidence: scratch/logs/texture_storage_reuse_probe.log.

### Dead end (2026-08-25)
Replacing XXH3 streaming state with the one-shot function for single-span textures did not resolve an improvement in an interleaved same-process A/B: experimental arm +0.45 ms (10.25 vs 9.80 ms over 44/44 frames) against a 2.86 ms resolution threshold. The shortcut and temporary A/B were removed. Evidence: scratch/logs/texture_single_span_ab.log.

### Resolution (2026-08-25)
Root cause: the cache had no way to know which guest bytes changed, so it verified all of them every frame - 49.39 MiB re-read and re-hashed per chapter-45 frame. Fix: Linux soft-dirty page tracking (runtime/guest_dirty_pages.{h,cpp}). The tracker proves itself at Open by demonstrating both a clean page staying clean and a written page turning dirty against /proc/self/pagemap; every doubt hashes; queries union all four alias windows so writes through any guest view are seen; a forced full re-hash every 64 generations bounds what the documented clear/write race can hide and is where a contradiction would be counted (auto-disabling skips above 32). Within-frame dedup and one-shot-XXH3 remain recorded dead ends below. Measured on the 101-frame chapter45_recovered replay: 244 of 244 textures skipped as page-clean (49.39 MiB not re-read), frames byte-identical to the always-hash control, forced verification found 0 contradictions in 244 checks. Interleaved A/B (GEARS_DRAW_AB_TEXDIRTY): experimental arm -1.27 ms draw loop (17.45 vs 18.72 over 44/44 frames) beyond a 0.66 ms resolution floor. Evidence: scratch/logs/texdirty_base.log, texdirty_exp101.log, texdirty_ab.log.

### Note (2026-08-25)
Live-run validation (2026-08-25): a real headless boot with the skip active armed the tracker across all four alias windows and played the startup movie - whose YUV planes are rewritten in place every frame, the exact catalog #53 hazard - with the 8 presented-frame dumps showing the animation advancing (5 distinct frames; first and last differ in content) and ZERO staleness MISSes. Three early MISSes in the first live attempt were a verification-order defect, not kernel lies: querying page bits BEFORE hashing misclassified any concurrent store landing between query and hash as a contradiction. Verification now hashes first and queries after, so a real late write reports its bit and reads as ordinary detection; only bytes changed while pages still read clean accuse the tracker. After the reorder: 0 misses across two further live boots. Evidence: scratch/logs/texdirty_live_on.log (pre-fix misses), texdirty_live_on2.log and texdirty_live_dump.log (post-fix, 0 misses), scratch/raw/texdirty_live/.

---
id: 137
title: Renderer texture staleness hashing consumes a double-digit share of CPU cycles
status: investigating
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

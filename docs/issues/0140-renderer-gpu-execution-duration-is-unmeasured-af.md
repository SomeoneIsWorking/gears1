---
id: 140
title: Renderer GPU execution duration is unmeasured after asynchronous frame slots
status: resolved
symptom: Live CPU preparation is 5-7 ms but no nonblocking GPU timestamp evidence exists, so neither an 8.33 ms/120 fps nor 16.67 ms/60 fps renderer budget is established
tags: performance,vulkan,gpu,timing,renderer
created: 2026-08-26
updated: 2026-08-27
---

## Root cause

The asynchronous renderer removed the producer's fence wait, so host wall/CPU
timing now ends at submission and cannot reveal how long the GPU executes the
frame.

## Next falsifier

Timestamp the complete per-frame command buffer with per-slot Vulkan queries,
collect only after that slot's fence signals, prove the instrument returns
nonzero and distinguishes materially different workloads, then measure a live
gameplay interval without adding a wait.

## Resolution

### Resolution (2026-08-27)
Per-slot Vulkan timestamp queries now bracket each complete renderer command buffer and are consumed only after that slot fence signals, before reuse. The chapter-45 replay measured 13.537 ms full versus 1.917 ms one-draw with zero failed queries, proving the instrument returns the other answer. A Release no-validation live headless run measured ordinary title frames at 7-8 ms GPU with zero failed samples; ordinary frames fit a 120 Hz GPU budget, but the captured heavy gameplay frame does not, and no complete native frontend exists.

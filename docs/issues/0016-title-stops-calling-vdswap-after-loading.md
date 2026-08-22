---
id: 16
title: title stops calling VdSwap after loading
status: resolved
symptom: post-load presentation stalls while the command processor keeps replaying work
tags: gpu,presentation,predication
created: 2026-07-22
updated: 2026-08-22
---

## Root cause

The command processor ignored Xenos packet predication. Masked command-buffer
regions executed as if selected, including completion interrupts that enqueued
more work. That feedback loop repeatedly replayed one recorded frame and made
guest presentation appear frozen.

## Resolution

The command processor now owns the bin mask and selection registers and consumes
but does not execute a predicated type-3 packet when their intersection is zero.
This fixed the packet semantics rather than rate-limiting work. The heavy phase
rose from roughly 0.42 fps to roughly 30 fps, while the movie phase remained
unchanged. Per-frame indirect submissions collapsed from thousands to the
expected bounded unit, with no stuck-wait diagnostics.

Later heap exhaustion was a separate allocator defect tracked and resolved in
catalog issue 17. A subsequent 480-second headless run reached 13,860 submitted
frames at about 29.3 fps with no heap error or stall.

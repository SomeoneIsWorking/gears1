---
id: 150
title: Native RHI vertex state misses a gameplay reset owner
status: resolved
symptom: At the gameplay transition the semantic tracker retains vertex stream slot 1 while the retained device state has cleared it, causing every later bound draw to mismatch
state_items: S003
tags: native-rhi,state,vertex-stream,re,gears1
created: 2026-08-28
updated: 2026-08-28
---

## Root cause

Gears 1 function `0x82487510` performs a device-state reset. Its retained body directly clears all
16 vertex-stream object slots at device `+0x2F9C` and the corresponding stride bytes at `+0x2FE0`
without calling the normal `0x8222AE20` binder. The semantic tracker therefore retained the last
slot-1 binding across the gameplay transition while the guest device correctly held zero.

## What was tried / dead ends

The two render-ring producer identities were initially considered, but the existing overlap
instrument had observed zero concurrent producer critical sections. A draw-time reconciliation
would have hidden the missing owner and destroyed event ordering. A binder-paused one-shot page
watch instead attributed the first slot-1 clear to host RIP `0x1ADE5A6`, which resolves to retained
guest function `0x82487510`.

## Resolution

The exact Gears 1 reset wrapper super-calls `0x82487510`, reads the device through its retained
global at `0x82BECBA0`, and publishes one title-neutral 16-slot reset event with independent
post-call state evidence. `RhiSemanticStateTracker` applies the reset as an owned range transition.
Focused controls cover match, missing, mismatch, partial-range erase, and ordered event delivery.

The corrected long headless transition run produced zero RHI semantic errors beyond the point where
the old tracker had emitted 154,333 stale-slot draw mismatches. A separate logging defect in the
ring identity probe was also removed: it now reports each distinct thread once instead of treating
every ordinary alternation as a newly discovered second thread.

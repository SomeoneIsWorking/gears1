---
id: C086
kind: claim
status: holds
created: 2026-08-27
tags: native-rhi,binding
depends: runtime/rhi_semantic_stream.cpp#CompareRhiBindingState, runtime/titles/gears1/rhi_bindings.cpp#sub_82220858, runtime/titles/gears1/rhi_bindings.cpp#sub_82222808, runtime/titles/gears1/rhi_bindings.cpp#sub_82222B98
reconfirmed: 2026-08-30
verified_at: 2026-08-30 05:10:12
---

## Claim

In the observed Gears 1 headless run through frame 120, all 970 texture and shader setter calls matched the post-call device object written by the retained recompiled body.

## Evidence

scratch/logs/rhi-bindings-debug.log: 734 texture, 118 pixel-shader, and 118 vertex-shader matches; zero missing or mismatched. tests/test_rhi_semantic_stream.cpp proves the comparator can return mismatch and preserves cross-kind ordering.

## What would falsify it

Any same-run missing or mismatched binding state, evidence that the device offsets differ for this executable identity, or a negative control that fails to report mismatch.

## Re-confirmed 2026-08-30

Committed c19182d headless frame-660 semantic report matched all 40,306 binding calls, including 13,352 texture and 9,877 calls each to pixel- and vertex-shader setters, with zero missing or mismatched post-call state.

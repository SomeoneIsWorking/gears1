---
id: C090
kind: claim
status: holds
created: 2026-08-27
tags: native-rhi,render-target,gears1
depends: runtime/titles/gears1/rhi_bindings.cpp#sub_8222B068, runtime/titles/gears1/rhi_bindings.cpp#sub_8222B398, runtime/rhi_semantic_stream.cpp#CompareRhiBindingState
---

## Claim

Gears 1 functions 0x8222B068 and 0x8222B398 bind colour render targets and the depth-stencil target, respectively; 0x8222B068 is not a vertex-stream setter

## Evidence

The retained 0x8222B068 body writes one of four device object slots at +0x2F88 and an interleaved descriptor shadow beginning at +0x2804; the retained 0x8222B398 body writes the adjacent fifth object at +0x2F98 and descriptor words at +0x2808/+0x28C0. A live slot-zero object carried EDRAM base 0x2d0. A headless frame-780 run matched 6,481 colour-target and 3,555 depth-target events with zero missing or mismatched observations across 83,573 bindings.

## What would falsify it

Falsified if a retained-body/device-layout read no longer maps these functions to the five target slots, a live object does not track EDRAM target state, or the independent descriptor-shadow comparison mismatches.

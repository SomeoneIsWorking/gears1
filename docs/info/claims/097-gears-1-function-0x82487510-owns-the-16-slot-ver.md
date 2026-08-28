---
id: C097
kind: claim
status: holds
created: 2026-08-28
tags:
depends: runtime/titles/gears1/rhi_device_state_reset_binding.cpp#sub_82487510, runtime/rhi_semantic_state.cpp#RhiSemanticStateTracker::ApplyVertexStreamReset
---

## Claim

Gears 1 function 0x82487510 owns the 16-slot vertex-stream reset that bypasses the normal binder

## Evidence

A binder-paused page watch caught the first direct slot-1 clear at host RIP 0x1ADE5A6, which resolves to retained guest function 0x82487510. Its body clears device offsets 0x2F9C through 0x2FDC and stride bytes at 0x2FE0. After adding the checked reset event, the transition run reached frame 3060 with zero RHI errors beyond the former frame-2987 first mismatch.

## What would falsify it

A direct clear of the same table is attributed to another owner without a corresponding semantic event, or a post-reset draw again carries stale semantic vertex streams.

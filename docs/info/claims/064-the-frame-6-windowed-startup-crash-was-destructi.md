---
id: C064
kind: claim
status: holds
created: 2026-08-14
tags: render,vulkan,lifetime
depends: runtime/gpu_renderer_capacity.cpp#EnsurePersistentCapacity
---

## Claim

The frame-6 windowed startup crash was destruction of a presenter-visible stage image when the renderer shrank its persistent EDRAM allocation, not a fault in SPIR-V generation.

## Evidence

Issue #110: the diagnostic printed 1280x1440 -> 1280x720 immediately before two identical createAccessChain/createBinOp crashes; capacity reuse survived 667 and 10109 frames and live input.

## What would falsify it

a build retaining the old exact-size teardown survives the 1440-to-720 transition, or the capacity build crashes at that transition with a valid image lifetime

---
id: C096
kind: claim
status: holds
created: 2026-08-28
tags:
depends: runtime/rhi_resource_reference.cpp#TryApplyNativeRhiReferenceFastPath, runtime/titles/gears1/rhi_resource_lifetime_binding.cpp#ExecuteResourceReference
---

## Claim

Non-boundary Gears 1 D3D AddRef and Release operations execute through the shared native big-endian atomic path and match retained refcount arithmetic

## Evidence

A live alternating run matched 12,000 of 12,000 resource-reference results with zero missing or mismatched evidence and measured about 51 ns native versus 72 ns retained mean execution. A default-native headless transition run reached frame 3060 with zero RHI errors. The focused concurrent test drives the shipping CAS implementation.

## What would falsify it

Any live A/B refcount mismatch, any zero-to-one or one-to-zero transition routed through the native fast path, or failure of the concurrent atomic test.

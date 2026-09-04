---
id: C096
kind: claim
status: holds
created: 2026-08-28
tags:
depends: runtime/rhi_resource_reference.cpp#TryApplyNativeRhiReferenceFastPath, runtime/titles/gears1/rhi_resource_lifetime_binding.cpp#ExecuteResourceReference
---

## Claim

In the historical static path, real Gears 1 leaf `0x8222E868` is the D3D resource AddRef entry and its observed non-boundary operations matched the shared native big-endian atomic implementation. This grounds the first dynarec discriminator's address and semantics; it does not prove xenonport execution.

## Evidence

A live alternating static-path run matched 12,000 of 12,000 resource-reference results with zero missing or mismatched evidence and measured about 51 ns native versus 72 ns retained mean execution. A default-native headless transition run reached frame 3060 with zero RHI errors. The focused concurrent test drives the same native CAS implementation intended for the Xenia-backed override.

## What would falsify it

Binary evidence assigns different semantics to `0x8222E868`, any live A/B refcount mismatch occurs, a boundary transition enters the native fast path, or the concurrent atomic test fails. Xenia execution remains separately unproven until the new discriminator runs.

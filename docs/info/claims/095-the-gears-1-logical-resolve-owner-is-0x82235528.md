---
id: C095
kind: claim
status: holds
created: 2026-08-28
tags: native-rhi,resolve,gears1
depends: runtime/titles/gears1/rhi_resolve.cpp#DecodeResolveCall, runtime/titles/gears1/rhi_resolve_binding.cpp#sub_82235528, runtime/rhi_semantic_stream.cpp#CompareRhiResolvePacket
---

## Claim

The Gears 1 logical resolve owner is 0x82235528 and its retained body emits one three-element auto-index rectangle-list copy whose source identity and destination address, pitch, and remaining height agree with the independently decoded title call.

## Evidence

Selective retained-body inspection plus focused decoder/packet controls and a headless GEARS_NATIVE_RHI_OBSERVE run through frame 540: 540 resolves, 540 matches, zero missing, zero mismatches.

## What would falsify it

A supported exact Gears 1 revision routes a logical resolve through another owner, or an observed call disagrees with the decoded source/destination or emitted rectangle packet.

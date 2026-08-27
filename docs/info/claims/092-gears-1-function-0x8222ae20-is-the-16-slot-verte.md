---
id: C092
kind: claim
status: holds
created: 2026-08-28
tags: native-rhi,vertex-buffer,gears1
depends: runtime/rhi_semantic_state.cpp#RhiSemanticStateTracker, runtime/titles/gears1/rhi_bindings.cpp#CaptureBoundVertexStreams, runtime/rhi_semantic_stream.cpp#CompareRhiDrawVertexState
reconfirmed: 2026-08-28
verified_at: 2026-08-28 00:48:35
---

## Claim

Gears 1 function 0x8222AE20 is the 16-slot vertex-stream binder and its semantic view is object identity plus offset-adjusted GPU address, remaining bytes, and byte stride

## Evidence

The retained setter consumes device, slot, buffer, offset, stride, and dirty-bit arguments. Its reset caller increments the slot from zero and exits when it reaches 16. The object table begins at device+0x2F9C and its 16 dwords end before the stride-byte table at +0x2FE0. A live 18-slot draw snapshot exposed the overlap by decoding stride bytes as phantom slot-17 objects 0x09000000/0x0A000000.

## What would falsify it

Falsified if a retained caller supplies slot 16 or greater as a valid stream, a bound draw consumes a different stream table, or the corrected 16-slot independent comparator reports a persistent real mismatch.

## Re-confirmed 2026-08-28

A corrected headless observation run through frame 780 matched all 3,715 bound-index draw snapshots against the independently decoded ordered 16-slot device state, plus all 24,239 draw packets, 86,061 bindings, and 780 presents, with zero missing or mismatched observations. The prior 18-slot implementation produced live slot-17 mismatches, so the instrument demonstrated both answers.

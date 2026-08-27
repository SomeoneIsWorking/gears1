---
id: C091
kind: claim
status: falsified
created: 2026-08-28
tags: native-rhi,vertex-buffer,gears1
depends: runtime/titles/gears1/rhi_bindings.cpp#sub_8222AE20, runtime/titles/gears1/rhi_vertex_buffer.cpp#DecodeVertexBufferView, runtime/rhi_semantic_stream.cpp#CompareRhiBindingState
falsified_on: 2026-08-28
---

## Claim

Gears 1 function 0x8222AE20 is the 18-slot vertex-stream binder and its semantic view is object identity plus offset-adjusted GPU address, remaining bytes, and byte stride

## Evidence

The retained body consumes r3..r8 as device, slot, buffer, offset, stride, and dirty bit; it reads buffer +0x18/+0x1C and writes object slots at device +0x2F9C, reverse-ordered address/range descriptors ending at +0x6F8, and stride/4 bytes at +0x2FE0. Its callers provide resource objects and strides such as 4, 12, and 20 bytes immediately before bound draws. A frame-780 headless run matched all 2,463 vertex-stream events against independent device-shadow decoding with zero missing or mismatched observations across 86,002 bindings.

## What would falsify it

Falsified if retained-body or caller evidence no longer has this argument/state contract, a bound draw consumes a different stream table, or the independent device-shadow comparator reports a mismatch.

## FALSIFIED 2026-08-28

The retained reset loop calls 0x8222AE20 for slots 0 through 15 and exits at 16. Decoding an eighteenth slot overlaps the stride-byte table at device+0x2FE0 and produced phantom 0x09000000/0x0A000000 objects in the live bound-draw comparator.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.

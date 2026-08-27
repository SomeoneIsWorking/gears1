---
id: C089
kind: claim
status: holds
created: 2026-08-27
tags: native-rhi,index-buffer
depends: runtime/titles/gears1/rhi_index_buffer.cpp#DecodeIndexBufferView, runtime/titles/gears1/rhi_bindings.cpp#CaptureIndexBufferView, runtime/titles/gears1/rhi_bindings.cpp#sub_8222DE50, runtime/rhi_semantic_stream.cpp#CompareRhiDrawPacket
---

## Claim

The Gears 1 bound index-buffer semantic view matches the retained draw packet at scale

## Evidence

Focused controls reject changed DMA address, size, width, endian mode, view fields, and missing evidence. A real headless GEARS_NATIVE_RHI_OBSERVE run through frame 780 matched 3717/3717 bound-index draws and 3926/3926 index-buffer bindings with zero missing or mismatched observations; the complete run matched 24233 draws and 80023 bindings.

## What would falsify it

Any change to the exact executable identity, index-buffer object decoder, draw-slice calculation, bound-index wrapper, or DRAW_INDX packet decoder; or any real run producing a missing/mismatched bound-index observation.

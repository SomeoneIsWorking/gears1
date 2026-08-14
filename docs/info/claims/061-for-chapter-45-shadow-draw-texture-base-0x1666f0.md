---
id: C061
kind: claim
status: holds
created: 2026-08-14
tags: render,oracle,texture
depends: runtime/gpu_draw_vertexfetch.cpp#DumpVertices
---

## Claim

For chapter-45 shadow draw texture base 0x1666F000, native and oracle upload identical canonical BC1 bytes: 32 bytes, FNV-1a 683FAB1B2803B4D0.

## Evidence

Native scratch/logs/native_upload_bc1.log and oracle scratch/logs/oracle_upload_bc1_5.log / oracle_upload_bc1_fixed.bin; oracle readback is after the same barrier used by the buffer-to-image copy, with an 18,822-load zero-match negative.

## What would falsify it

Either shipping upload path changes, or a post-barrier repeat for this draw produces different length or hash.

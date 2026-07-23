---
id: 24
title: Hot-pair DRAW_INDX captured: full-screen quad, geometry from vfetch const #95 + int32 index buffer
status: resolved
symptom: need the hot-pair draw parameters (prim type, index count/format/address, vertex base/stride) to feed a pipeline; command processor executed DRAW_INDX as state-only and captured no draw params
tags: gpu,draw,pm4,draw_indx,vertex-fetch,geometry
created: 2026-07-23
updated: 2026-07-23
---

Captured a representative hot-pair (vs_5363d074/ps_501ac5d8) DRAW_INDX on a real headless run (runtime/vd_null_gpu.cpp CaptureHotDraw, GEARS_DRAW_CAPTURE=1; report scratch/draw-params/hot_draw.txt). Packet layout decoded per Xenia pm4_command_processor_implement.h (ExecutePacketType3_DRAW_INDX -> viz token then ExecutePacketType3Draw) + registers.h VGT_DRAW_INITIATOR (prim_type[0:5], source_select[6:7], index_size[11], num_indices[16:31]) + VGT_DMA_SIZE (num_words[0:23], swap_mode[30:31]).

CAPTURED: VGT_DRAW_INITIATOR=0x00060804 -> prim_type=triangle_list(0x4), source_select=kDMA/indexed(0), index_size=int32(1), num_indices=6. VGT_DMA_BASE=0x978d0, VGT_DMA_SIZE=0x8000000c (num_words=12, swap_mode=2/k8in32). Index buffer guest_base 0x978d0, 6x int32.

GEOMETRY SOURCE (recovered from register file, NOT the packet): the hot VS fetches vertices from the shared-memory SSBO via vertex fetch constant #95. Xenia disasm prints 'vf0' = 95 - storage_index; spirv-dis confirms the shader reads fetch_constants[0][47][2..3] = fetch-file dwords 190,191 = reg 0x48BE/0x48BF. At draw time reg 0x48BE=0x00097813 0x100000c2 -> type 3 vertex fetch, base 0x97810 (dword addr 0x25E04), size 48 words (192 bytes), endian 2. Stride=12 dwords (48 bytes) is baked into the patched microcode (catalog #22), verified from Xenia disasm of the captured ucode.

VERIFIED COHERENT (all cross-consistent, not garbage): indices = 0 1 3 0 3 2 (two triangles of a quad), min 0 max 3; vertex buffer 192 bytes = exactly 4 vertices so max index 3 is in range; the 4 vertices are a clean NDC full-screen quad (-1.0,1.0)(1.0,1.0)(-1.0,-1.0)(1.0,-1.0) with z=0 w=1; vertex buffer 0x97810..0x978d0 (192 bytes) abuts the index buffer at 0x978d0 -- contiguous. Matches the system-constants prediction of a full-screen 1280x720 RT-sampling pass.

Minor open: VGT_DMA_SIZE.num_words=12 -> Xenia length 12*4=48 bytes vs 24 for 6 int32 indices; num_indices=6 is the authoritative draw count. kImmediate/kAutoIndex source paths not exercised by this draw (capture handles+reports them if hit).

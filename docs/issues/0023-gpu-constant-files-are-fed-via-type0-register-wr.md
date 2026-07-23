---
id: 23
title: GPU constant files are fed via TYPE0 register writes + LOAD_ALU_CONSTANT, not ring SET_CONSTANT; hot-pair draw has textures but no vertex-fetch constant
status: resolved
symptom: translated shaders need constant UBOs (ALU float 0x4000, fetch 0x4800, bool/loop); command processor skipped SET_CONSTANT/LOAD_ALU_CONSTANT so what path actually supplies them was unverified
tags: gpu,constants,fetch,set_constant,xenia,cmd-processor,draw
created: 2026-07-23
updated: 2026-07-23
---

Implemented the sequencer constant-file loads in runtime/vd_null_gpu.cpp (TrackConstantLoad) mirroring Xenia (pm4_command_processor_implement.h ExecutePacketType3_SET_CONSTANT/_SET_CONSTANT2/_LOAD_ALU_CONSTANT/_SET_SHADER_CONSTANTS; base offsets from command_processor.cc WriteALURangeFromRing etc and register_table.inc):
- SET_CONSTANT 0x2D: index=offset_type&0x7FF, type=(offset_type>>16)&0xFF; type0 ALU->0x4000, 1 FETCH->0x4800, 2 BOOL->0x4900, 3 LOOP->0x4908, 4 REGISTERS->0x2000; count-1 dwords.
- LOAD_ALU_CONSTANT 0x2F: [addr&0x3FFFFFFF][offset_type][size&0xFFF]; same type map; reads size dwords from physical mem.
- SET_CONSTANT2 0x55 / SET_SHADER_CONSTANTS 0x56: raw index&0xFFFF, direct.
Handled in the fetch()-based packet path (not HandleType3's 20-word copy) because a SET_CONSTANT can carry the whole 1024-dword ALU file. Verified with GEARS_CONST_DUMP=1.

FEED CENSUS (cumulative over 950+ frames, and at the hot-pair vs_5363d074/ps_501ac5d8 DRAW_INDX):
- ring SET_CONSTANT type-0 ALU / type-1 FETCH: 0. type-2/3 BOOL/LOOP: 0. type-4 REGISTERS: ~38/draw.
- LOAD_ALU_CONSTANT type-0 ALU (from memory): ~12/hot-pair-draw. type-1 FETCH: 0.
- SET_CONSTANT2 / SET_SHADER_CONSTANTS: 0.
- TYPE0 register writes into 0x4000..0x47FF: ~1.16M; into 0x4800..0x48FF: ~116k.
=> The title supplies ALU-float and fetch constants as plain TYPE0 register writes (the D3D deferred-state draw flush), which the command processor ALREADY applied. The genuinely-dropped-before packets were LOAD_ALU_CONSTANT (12 ALU-from-memory loads per hot-pair draw) and SET_CONSTANT type-4; both now applied.

VERIFICATION (reproducible across 3 runs):
- ALU float file (0x4000): 44 non-zero dwords; c0-c3 = identity, c4=(-1.2094363,~0,0,0), c5=(~0,-1.2094363,0,0) = a screen/viewport transform. Matches the hot VS's own disassembly (mul/mad oPos from c0-c3). Well-formed.
- Fetch file (0x4800), classified per 6-dword slot by dword_0 type: slot0 = texture, base 0xbde0000, 1280x720, pitch 1280, format 0x20, tiled (a render target); slots1,2 = textures base 0x960000/0x9b0000, 640x360, format 0x2. Plausible guest addresses, valid formats, sizes decode to real screen resolutions.

OPEN (belongs to draw-params/draw-backend, NOT a capture defect): the hot VS reads vfetch_full ...vf0 (Stride=12) but the fetch file at draw time contains ZERO type-3 (vertex) fetch constants -- only the 3 textures. So the vertex base is not in the fetch register file. Consistent with catalog #22 (the vfetch instruction is patched with the fetch info at bind time). Xenia itself defers validating vertex-vs-texture fetch type (spirv_shader_translator_fetch.cc:67). The hot pair looks like a full-screen pass sampling a 1280x720 RT. Where its geometry comes from is the next step's problem.

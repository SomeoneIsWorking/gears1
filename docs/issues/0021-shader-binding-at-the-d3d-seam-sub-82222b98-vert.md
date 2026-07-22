---
id: 21
title: Shader binding at the D3D seam: sub_82222B98 (vertex) and sub_82222808 (pixel)
status: resolved
symptom: cannot find where the title sets shaders; which shaders does it actually bind at runtime
tags: gpu,shaders,d3d,re,binding
created: 2026-07-23
updated: 2026-07-23
---

The shader-set entry points, and the point at which a shader really becomes
bound, are now both identified. They are two different places, and only the
second is authoritative.

## The D3D API setters

    sub_82222808  D3DDevice_SetPixelShader   r3 device, r4 shader object
                  stores r4 into device+0x3080, dirty bits 1<<52 and 1<<49 at device+0x10
    sub_82222B98  D3DDevice_SetVertexShader  r3 device, r4 shader object
                  stores r4 into device+0x3084, dirty bit 1<<51 at device+0x10

How they were found, in order:

1. A static bl-scan over the flat image gives the 51 functions in
   0x82218000-0x82241000 that are called from the UE3 RHI zone
   (0x82540000-0x82560000).
2. Every one of the 51 got the same runtime argument probe
   (GEARS_SHADER_ARGSCAN=1, runtime/hle_d3d.cpp): scan r3..r10 for a pointer to
   an object containing the 0x102A11tt container word, and record which
   register, at which offset inside the object, and which shader type.
   Candidates are eliminated by measurement, not by reading.
3. Measured over ~4 minutes: sub_82222808 and sub_82222B98 are called exactly
   the same number of times (410374 each), sub_82222808 always receives a
   type-0 (pixel) object in r4 and sub_82222B98 always a type-1 (vertex) one.
4. The static side agrees: 0x82222890 is `stw r29, 0x3080(r30)` and 0x82222C40
   is `stw r29, 0x3084(r30)`, and the shader-flush emitter at 0x82234xxx reads
   both (`lwz r31, 0x3084(r30)` / `lwz r29, 0x3080(r30)`) and emits the
   sequencer loads from them.

**The D3D shader object is NOT the bare container.** The container word sits at
+0x28 inside a pixel-shader object and at +0x368 inside a vertex-shader one, and
the object carries further fields past 0x380. A detector that expects the magic
at offset 0 finds nothing -- the first version of this scan reported zero hits
on both setters for exactly that reason.

Secondary consumers of shader objects the same scan turned up, unidentified:
sub_82222350 (r5, +0x388), sub_8222AFD8 / sub_82229460 / sub_82229398
(r6/r7, +0x3C8 and +0x3E8), sub_8222B068 / sub_8222B398 (r7, +0x158).

## The authoritative bind point: PM4 sequencer loads

Hooking the setter is not enough, because the microcode the GPU executes is not
the microcode in the container -- see the companion entry on fetch patching. The
point where a shader really becomes bound is the sequencer instruction-memory
load in the command stream:

    IM_LOAD            (TYPE3 opcode 0x27) -- microcode by physical address
    IM_LOAD_IMMEDIATE  (TYPE3 opcode 0x2B) -- microcode inline in the packet

Both are present in this title's stream (measured: ~181 and ~33 per frame).
runtime/vd_null_gpu.cpp captures them under GEARS_SHADER_CAPTURE=1 and writes
the microcode to GEARS_SHADER_CAPTURE_DIR (default scratch/shaders/bound),
with a manifest of load counts. This covers every path including the movie
player's hand-built command buffer, and needs no knowledge of the D3D API.

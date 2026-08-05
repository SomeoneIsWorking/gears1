---
id: 70
title: The renderer copied 80 KiB of register file FOUR TIMES PER DRAW; a quarter-gigabyte of memcpy per frame
status: resolved
symptom: renderer drops 6-12 frames a second as 'still busy'; visible judder; 27-35 ms per frame with the draw loop dominated by 'modification derivation'
tags: perf,gpu,draw-loop,fixed
created: 2026-08-05
updated: 2026-08-05
---

## The symptom

A live Act 1 walk reported the renderer dropping frames the guest had already
submitted:

    guest-draw: 17.9 frames/s rendered, 11.9/s dropped as the renderer was still
                busy (35 ms/frame in RenderFrame, of which 27 ms on-core)

The guest submits at 29.9 fps, so up to 40% of frames never rendered. That is
judder, and it is invisible to every still frame this project captures -- which is
why it survived a session of frame-by-frame analysis. It only shows up in the
consecutive-frame numbering of a live run: 28 -> 30 -> 32 -> 34, every other frame
missing.

## The cause

The draw loop's own breakdown named it:

    draw loop 25 ms = state+pipeline 11 (modification derivation 8 ...) + uniforms 6
                    + index prep 1 + record 6 + unattributed 1

Four entry points in gpu_draw_xlate.cpp each began with

    RegisterFile regs;
    std::memcpy(regs.values, registerFile, kRegisterCount * sizeof(uint32_t));

to read about ten registers.  is 0x5003, so that is
**80 KiB per call**. All four -- DeriveShaderModifications, DeriveSystemConstants,
DeriveViewport and IsPrimitivePolygonal -- are called PER DRAW, and an Act 1 frame
issues ~810 draws.

**~320 KiB per draw, about 260 MiB of memcpy per frame**, purely to read a handful
of dwords.

## The fix

Alias the caller's array instead of copying it. RegisterFile is standard-layout
with  as its only data member, so the array the caller already holds IS a
RegisterFile. Three static_asserts fail the build if that ever stops being true,
rather than letting it decay into a silent aliasing bug.

## Result, measured on the same walk on the same machine

    before   27-35 ms/frame, 19-27 ms on-core, 17.9-29.9 fps, up to 11.9/s dropped
    after    16-18 ms/frame,  8-9  ms on-core, 28.9-29.9 fps,   0-1.0/s dropped

Modification derivation went from 8-9 ms to 0. The draw loop went from 24-27 ms to
9-11 ms. The renderer now keeps pace with the guest.

**The rendered image is pixel-identical** to the pre-change output on a replayed
capture, and all 24 tests pass -- this changes only how the registers are reached,
never what is read.

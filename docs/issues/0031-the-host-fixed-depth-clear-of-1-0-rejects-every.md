---
id: 31
title: The host-fixed depth clear of 1.0 rejects every reverse-Z draw
status: open
symptom: a draw whose geometry survives clipping still produces 0 fragment invocations; the frame's draws test GEQUAL and the guest's viewport is reverse-Z (PA_CL_VPORT_ZSCALE=-1, ZOFFSET=1)
tags: gpu,draw,draw-backend,depth,reverse-z,clear,gameplay
created: 2026-07-27
updated: 2026-07-27
---

CONFIRMED by A/B on byte-identical input (tools/frame_replay over
scratch/frames/act1.gfr, GEARS_DRAW_DIAG per-draw table), not inferred.

The depth clear in runtime/gpu_draw.cpp is a HOST CONSTANT of 1.0, with
GEARS_DRAW_DEPTH_CLEAR=<float> as its control arm. That was harmless while only
menu frames rendered. It is fatal for the gameplay frame: 390 of the 391 draws on
the HDR world surface program PA_CL_VPORT_ZSCALE = -1 and ZOFFSET = 1, which is
reverse-Z (near maps to 1.0, far to 0.0), and 390 of them test depth GEQUAL.
Against a buffer cleared to 1.0, GEQUAL passes only at exactly the near plane, so
every fragment is rejected.

The two draws in that frame whose geometry survives clipping (see issue #30, which
is why only two do) show it cleanly -- same input, only the clear differs:

    depth clear 1.0   draw 216: 217 prims after clip, fragment invocations 0
                      draw 413:  57 prims after clip, fragment invocations 0
    depth clear 0.0   draw 216: 217 prims after clip, fragment invocations 655360
                      draw 413:  57 prims after clip, fragment invocations 220587

So the geometry, the pipeline, the constants and the shading were all fine for
these draws; the depth clear alone decided between "nothing" and 875947 shaded
fragments.

This is NOT to be fixed by changing the constant to 0.0 -- that is the same
defect with a different magic number, and a frame that mixes conventions would
break again. The real fix is to take the clear from the GUEST: on Xenos a clear
is a kCopy draw whose RB_COPY_CONTROL selects a clear, with the values in
RB_DEPTH_CLEAR (0x231D) and RB_COLOR_CLEAR (0x231C). Those registers are already
in the per-draw snapshots the frame capture carries, and the render-target cache
already recognises kCopy draws -- so the clear belongs on the same path as the
resolve, per surface, instead of being a host constant applied once per frame.

Blocked on nothing. Ordered AFTER #30 only because #30 is why just two draws in
this frame can demonstrate it.

---
id: 30
title: World geometry on the HDR surface is killed by clipping, not by shading or depth
status: investigating
symptom: an Act 1 gameplay frame shows the HUD over a flat grey field; EDRAM surface 0x400 (the 7e3 HDR world, ~390 colour draws) changes 0 of 921600 pixels
tags: gpu,draw,draw-backend,frame,clip,viewport,gameplay,edram
created: 2026-07-27
updated: 2026-07-27
---

MEASURED with the new per-draw diagnostic table (GEARS_DRAW_DIAG=<path.tsv>) on a
captured Act 1 frame replayed offline (tools/frame_replay, scratch/frames/act1.gfr),
so every arm below rendered BYTE-IDENTICAL input.

Verdicts by EDRAM surface, 722 issued draws:

    0x0     rasterised_no_fragment      7
    0x2d0   killed_by_clip_or_cull    231   colour_fully_masked   1
            rasterised_no_fragment     78   shaded                9
    0x400   killed_by_clip_or_cull    389   rasterised_no_fragment 2
    0x5a0   shaded                      5

**389 of surface 0x400's 391 draws assemble primitives and lose every one of them
to clip/cull.** They are not shading black, they are not depth-rejected, they are
not masked: `ia_prims` is non-zero and `prims_after_clip` is 0. This falsifies
both suspects previously recorded on re-frontier `gameplay-scene`:

  - NOT the depth-only draws (catalog #29). Surface 0x400 is 348-390 kColorDepth
    draws and exactly ONE kDepthOnly. Fixing the depth-only contract left the
    presented frame BYTE-IDENTICAL (0 of 921600 pixels changed).
  - NOT the resolve-sampling chain. These draws die before rasterisation, so what
    they would have sampled cannot matter.

Cull is ruled out as the mechanism, not merely unlikely: the host rasteriser is
still fixed to VK_CULL_MODE_NONE, which can only keep MORE primitives than the
guest asked for (the guest sets cull_back on 380 of them). So the loss is
CLIPPING -- the transformed positions are outside the clip volume.

State that is NOT the difference (identical between the dead world draws and the
14 draws that do shade): PA_CL_VTE_CNTL 0x43f, viewport scale/offset
640/639.75/-360/360.25 (correct for 1280x720), clip not disabled (PA_CL_CLIP_CNTL
bit 16 clear on both).

State that IS specific to the world draws:

  - **PA_SC_WINDOW_OFFSET is non-zero on half of them.** 195 of 391 carry
    0x7e000000, which decodes to window_y_offset = -512; the other 196 carry 0.
    With viewport heights of 720 and 208 (720 = 512 + 208) this is the frame
    being rendered in TWO predicated tiles, rows 0..511 and rows 512..719.
    `vtx_window_offset_enable` (PA_SU_SC_MODE_CNTL bit 16) is set on all of them.
    We DO route this through Xenia's own draw_util::GetHostViewportInfo and
    GetScissor, which apply it (draw_util.cc:347-350), and both our call sites
    (DeriveViewport and DeriveSystemConstants) pass matching args -- so the
    offset reaches the viewport and the shader's ndc_scale/ndc_offset
    consistently. Ruled out as a MISMATCH; still unverified as CORRECT.
  - PA_CL_VPORT_ZSCALE = -1, ZOFFSET = 1 on 390 of 391: reverse-Z. See issue #31,
    which is the second, independent defect stacked behind this one.

NOT yet done, and the next step: dump the vertex shader's own transformed clip
space positions per draw. The table names the stage; only the positions name the
cause. Every other candidate above has been eliminated from the register state.

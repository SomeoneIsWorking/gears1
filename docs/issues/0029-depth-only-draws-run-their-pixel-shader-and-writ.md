---
id: 29
title: Depth-only draws run their pixel shader and write colour
status: open
symptom: 269 of an Act 1 gameplay frame's 743 draws carry RB_MODECONTROL.edram_mode == kDepthOnly (5); the whole-frame guest-draw backend issues them with a pixel shader bound and colour writes enabled
tags: gpu,draw,draw-backend,frame,edram,depth
created: 2026-07-27
updated: 2026-07-27
---

MEASURED, not yet fixed. Census added to runtime/gpu_draw.cpp (frame EDRAM modes line), Act 1 gameplay frame reached with tools/capture_gameplay_frame.sh:

    frame EDRAM modes (RB_MODECONTROL.edram_mode): color_depth=456 depth_only=269 COPY(resolve)=18

Xenia's contract (xenos.h EdramMode, vulkan_command_processor.cc IssueDraw): the
pixel shader is used ONLY when edram_mode == kColorDepth. Xenia's own comment
records the evidence for treating kDepthOnly as 'pixel shader never used',
including titles whose kDepthOnly draws bind complex shaders that clearly belong
to the colour pass.

We bind and run the pixel shader for all 269, so every one of them shades and
writes colour into its surface's host target. That is 36% of the frame's draws
painting colour they must not.

NOT fixed in the render-target-cache change deliberately: it is a separate
behaviour change and deserves its own A/B measurement rather than being folded
into the routing change. The fix is to build the pipeline with no fragment stage
when edram_mode == kDepthOnly, as Xenia does by leaving pixel_shader null.

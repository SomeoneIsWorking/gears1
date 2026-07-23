---
id: 28
title: Rendered scene frame is tinted pink/red and split by a hard diagonal
status: resolved
symptom: the guest-draw whole-frame backend renders recognisable game imagery, but everything is tinted pink/red and a hard diagonal edge cuts across the frame, with half of each full-screen rectangle missing
tags: gpu,draw,draw-backend,frame,vulkan,color,swizzle,rectangle-list,primitive
created: 2026-07-23
updated: 2026-07-23
---

Follows #27, which is resolved: the frame is no longer black. scratch/screenshots/fixed/frame.png (headless, GEARS_NO_WINDOW=1, GEARS_DRAW_FRAME=1 GEARS_DRAW_FRAME_AT=600 GEARS_DRAW_RT=1) is unmistakably the Gears of War title screen -- logo, PRESS START prompt, Epic copyright line, wall art. 919796-921600 of 921600 px non-black (99.8%-100.0%) over 4 runs.

TWO SEPARATE DEFECTS ARE VISIBLE, and they should not be conflated:

1. THE DIAGONAL SPLIT. A straight edge runs corner to corner; on one side the wall art is bright, on the other it is dark. This is kRectangleList (VGT_DRAW_INITIATOR prim_type 0x11) still falling through to VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST. The Xenos rectangle list gives 3 vertices and the hardware SYNTHESIZES the 4th; a plain triangle list draws only the first triangle, i.e. exactly half the rectangle, split along the diagonal. This frame carries 19 rectangle_list draws, several of them full-screen post-process passes, which is why the split lands on the composite. THE FIX IS A SYNTHESIZED 4TH VERTEX, NOT AN INDEX REMAP -- see Xenia's PrimitiveProcessor for how the missing corner is derived. This is already named as a gap on re-frontier step draw-backend-frame.

2. THE PINK/RED TINT. NOT YET DIAGNOSED -- do not guess a swizzle and check whether it looks better. Candidates, in the order they should be tested, each with a measurement rather than an eyeball:
   - The host colour target's VkFormat vs the guest's RB_COLOR_INFO color_format for this surface. The backend uses one fixed host target for all 8 of the frame's resolve destinations (a known conflation, re-frontier draw-backend-rt).
   - Gamma. kSysFlag_ConvertColor0ToGamma is only set when RB_COLOR_INFO.color_format is k_8_8_8_8_GAMMA; confirm what the frame's surfaces actually declare.
   - The per-binding component swizzle: DecodeGuestTexture composes the guest swizzle with the host format's component order and applies it through the VkImageView component mapping. A red/blue swap there would tint everything, and the decoded blobs tools/decode_bc.py renders are the control -- they came out correct, so if the tint is a swizzle it is on the UPLOAD/view side, not the decode.
   - color_exp_bias from RB_COLOR_INFO (measured non-zero and derived correctly for #27; re-check per surface).

Useful arms already in the tree: GEARS_DRAW_ONLY=N (emit one draw over the clear), GEARS_DRAW_STATS=1 (per-draw pipeline statistics), GEARS_DRAW_FRAME_STEP=N (checkpoint images), GEARS_DRAW_FRAME_LIST=1 (per-draw census with fetch bases, blend/mask/depth and float-constant summary), GEARS_DRAW_TEX_DUMP=1 + tools/decode_bc.py (decode control).

RESOLVED. THE TWO DEFECTS WERE ONE DEFECT, and the entry above was wrong to
present the tint as independent -- worth recording, because "two symptoms, two
causes" was an assumption, not a measurement.

kRectangleList draws are the full-screen colour-grade/darkening passes on this
screen. Drawn as a plain triangle list, each covered only half its rectangle, so
the UNCOVERED half showed the untinted wall art -- that bright half is what read
as "a pink/red tint", and the boundary between graded and ungraded is the
diagonal. Expanding the rectangles removed both at once.

THE FIX: a geometry shader, ported from Xenia's
VulkanPipelineCache::GetGeometryShader (kRectangleList branch), in
runtime/gpu_draw_xlate.cpp (BuildRectangleGeometryShader) and wired in
runtime/gpu_draw.cpp. The fourth vertex cannot be synthesized in the index
buffer: its ATTRIBUTES are mirrored across the longest edge just as its position
is, so the expansion has to run after the vertex shader. Xenia's VS-expansion
fallback (kRectangleListAsTriangleStrip) is an unimplemented TODO in its SPIR-V
translator; the geometry shader is the path that actually works.

MEASURED, headless, GEARS_DRAW_FRAME_AT=600, 3 runs: 19 of 19 rectangle_list
draws expanded (2 distinct geometry shaders -- 1 and 0 interpolators), 171-174 of
171-174 draws issued, 0 skipped, 917090-921600 of 921600 px non-black
(99.5-100.0%). Vulkan validation clean for the geometry stage. Zero pixels match
the pink test (r>150 and b>g+40) anywhere in the frame, against a whole diagonal
half before. scratch/screenshots/rect/frame.png and frame2.png are the title
screen with no diagonal and no pink.

Also fixed in the same change: the pipeline cache was keyed on the microcode
HASHES, which stopped identifying a stage once one microcode began translating
to several shaders (one per modification, since #27). It is keyed on the module
handles now.

STILL NOT ESTABLISHED: whether the wall's overall red saturation is exactly the
guest's. It is no longer a visible defect, and the candidates in the list above
were never tested, so this is an open question rather than a known bug.

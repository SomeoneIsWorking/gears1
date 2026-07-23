---
id: 27
title: Scene frame renders 176/176 draws but is black: cause is stub textures, not the render target
status: open
symptom: post-load scene frame issues every draw yet the screenshot is entirely black (0 non-black px); earlier the whole frame went black from draw 2
tags: gpu,draw,draw-backend,frame,vulkan,textures,rt,blend,colormask,descriptors,radv
created: 2026-07-23
updated: 2026-07-23
---

Continuation of #26. A real POST-LOAD SCENE frame is now reachable and renders. Draws-per-frame profiled over ~4300 frames of one headless run (GEARS_DRAW_FRAME=1 with GEARS_DRAW_FRAME_AT set past the run length, which logs one line per frame): loading frames carry 2-3 draws; the scene phase starts at frame 571 and holds 168-186 draws/frame thereafter. Frame 600 chosen.

MEASURED (headless, GEARS_NO_WINDOW=1, GEARS_DRAW_FRAME_AT=600):
- 173-176 of 173-176 draws issued, 0 skipped. (The exact count varies 169-176 run to run because frame boundaries are not deterministic; every run issues 100% of what it captured.)
- 22 distinct shader pairs, 26 distinct shaders, 28 pipelines, 6 texture descriptor layouts, 5 pipeline layouts, 224 texture bindings.
- Primitives: point_list x48, triangle_list x93, rectangle_list x19, quad_list x16.
- RenderFrame blocks the CP thread ~520 ms (was 163-178 ms for the 26-draw loading frame). One-shot, so steady-state fps is unchanged.

THREE REAL DEFECTS had to be fixed to get there. Each was a hardcoded assumption that happened to be true for the 2-shader loading frame:

1. HARDCODED TEXTURE DESCRIPTOR SET LAYOUT -> RADV COMPILER CRASH (SIGSEGV, not a validation error).
   Sets 2/3 (Xenia kDescriptorSetTexturesVertex/Pixel) were created with a fixed 2 sampled images + 1 sampler. Xenia's translator decides that layout per shader: FindOrAddTextureBinding puts image i at binding i with an image type from the fetch dimension (1D/2D -> 2D ARRAY, k3DOrStacked -> 3D, kCube -> CUBE), and FindOrAddSamplerBinding puts sampler j at binding texture_count + j. The scene frame has a shader declaring 10 textures; a layout that does not match is undefined behaviour and crashed inside libvulkan_radeon lower_immediate_samplers (bt: radv_nir_lower_immediate_samplers <- radv_shader_spirv_to_nir <- radv_graphics_pipeline_compile).
   FIX: ShaderXlate now carries the translated shader's texture bindings (fetch constant + dimension) and sampler count; layouts are built per shader signature and cached, pipeline layouts per (vs sig, ps sig), with one 1x1 stub image per declared dimension.

2. 16-BIT INDEX BUFFERS UNDERSIZED. The index buffer was allocated indexCount*(indexIs32?4:2) bytes but ALWAYS bound as VK_INDEX_TYPE_UINT32, so every 16-bit indexed draw read twice its buffer. Caught by validation VUID-vkCmdDrawIndexed-robustBufferAccess2-08798 (10x, duplicate limit). FIX: always allocate 32-bit and widen guest 16-bit indices on the way in.

3. HARDCODED OUTPUT-MERGER STATE -> THE BLACK FRAME (the original symptom). The pipeline always wrote RGBA, always depth-tested LESS_OR_EQUAL with depth writes on, and never blended. Per-draw checkpoints (new GEARS_DRAW_FRAME_STEP=N) showed the whole frame going pure black at draw 2 of 173. The per-draw census (new GEARS_DRAW_FRAME_LIST=1) showed why: those early draws carry RB_COLOR_MASK = 0x0. They are depth-only passes the guest masked off entirely; we rendered them with a full colour write and the shaders' output is black.
   FIX: RB_COLOR_MASK (0x2104), RB_BLENDCONTROL0 (0x2201) and RB_DEPTHCONTROL (0x2200) are read from THAT DRAW'S OWN register snapshot and baked into the pipeline; the pipeline cache key gained the output-merger state. After the fix the first 30 draws behave correctly: 0 px differ from the clear after 10 draws (depth-only, as intended), then real geometry appears (6540 px at 20 draws, 7910 px at 30 -- scratch/screenshots/cp30.png shows guest polygons over the clear).

WHY IT IS STILL BLACK (the honest residual): from draw ~31 on, textured quad_list draws cover the screen and shade black because every texture is a 1x1 stub. This is the TEXTURE UPLOAD gap, and it is now demonstrated to be the blocker -- NOT the render target. Final frame: 0 of 921600 pixels non-black.

RT LINK (milestone step 3) IS IMPLEMENTED AND MEASURED, GEARS_DRAW_RT=1. Resolve destinations are read straight out of the frame's own snapshots (RB_COPY_DEST_BASE, 0x2319); a texture binding whose fetch constant base (0x4800 + fc*6, dword1 >> 12 << 12) matches one is bound to the rendered colour target instead of the stub. Vulkan forbids sampling the bound attachment, so the render pass is split at each such draw: end pass, copy colour -> a separate sampled image, resume with LOAD. Measured on frame 600: 8 resolve destinations (0x30c000 0x6e0000 0xba40000 0xbcc0000 0xbde0000 0xc2e0000 0xc7e9000 0xcb81000); 30 of 224 texture bindings (13.4%) served by the rendered RT, 194 stub; 10 segments, 9 snapshots; 176/176 draws issued. The hot pair of #23/#25 is draw 30 of this frame and its fetch constant 0 base IS 0xbde0000 -- that address is confirmed live in a scene frame.
It lights up NOTHING, because what it samples is itself black. Two fidelity gaps remain in the link and are NOT yet observable: all 8 resolve destinations are conflated onto one host colour target (needs a per-surface model keyed on RB_COLOR_INFO/RB_SURFACE_INFO with resolve EVENTS captured, not just draws), and no tiling/format conversion is applied.

NOT DONE / still host-fixed rather than guest-derived: cull mode (PA_SU_SC_MODE_CNTL), viewport and scissor.

NEXT STEP: texture upload. Nothing else in this chain can be judged while every sample returns the stub.

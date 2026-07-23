---
id: 27
title: Scene frame renders 176/176 draws but is black: cause is stub textures, not the render target
status: resolved
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

### Resolution (2026-07-23)
PARTIALLY RESOLVED, and the diagnosis in this entry is FALSIFIED as the sole cause.

Texture upload is DONE and VERIFIED (re_frontier draw-backend-textures). Fetch constants are decoded with Xenia's texture_util/texture_address/FormatInfo; endianness is an XOR on the source byte offset; the guest swizzle is composed with the host format's component order and applied via the VkImageView component mapping. MEASURED on frame 600: 26 distinct fetch constants, 20 uploaded (1.5 MiB), 176 of 218 texture bindings (80.7%) served by real guest data, 30 by the rendered RT, 12 by a stub. Formats: k_DXT4_5 x13, k_DXT1 x7, k_24_8_FLOAT x2, k_16_16, k_16_16_16_16_EXPAND, k_16_16_16_16_FLOAT, k_8_8_8_8 -- all 2D, all tiled, endian k8in16, identity swizzle. DXT1+DXT4_5 are 20 of 26.

THE DECODE IS VERIFIED ON REAL DATA, not merely plausible: GEARS_DRAW_TEX_DUMP=1 writes each decoded blob and tools/decode_bc.py renders it. scratch/screenshots/texdump/01caf000_k_DXT4_5_1024x512x1_tiled.png is unmistakable Gears world art (a grimy concrete/steel wall with grating shadows); 02eb1000_k_DXT1_256x256x1_tiled.png is a clean radial falloff map. Detiling, endian and block decode are right.

THE FRAME IS STILL BLACK (0 of 921600 px non-black), so 'stub textures' was necessary but not sufficient. Controlled A/B, 3 arms in one batch on identical code: real textures + guest viewport, real textures + host-fixed viewport, stub textures + guest viewport -- ALL give exactly 0 non-black. Depth is ruled out too: GEARS_DRAW_DEPTH_CLEAR=0.0 vs 1.0 and GEARS_DRAW_NODEPTH=1 all give 0.

WHAT THE CHECKPOINTS NOW SHOW, per draw: draws 1-28 light only 7910 of 921600 px (0.86%), in the top-left corner. Draws 29-31 are full-screen quad_list passes with RB_BLENDCONTROL0 = 0x1000400 = src*ZERO + dst*SRC_COLOR (a pure multiply) sampling the guest's own 256x32 k_DXT1 at 0x1f45000, which is genuinely near-black, so the target goes to (0,0,0). Draws 32-170 -- where this frame's actual world geometry lives, all quad_list with 2- and 10-texture shaders and additive/alpha blending -- add EXACTLY ZERO pixels on top of that. So the world draws are not rasterising, or are shading zero, upstream of the output merger. That is the open question now.

TWO MORE REAL DEFECTS were fixed while isolating this, both guest-derived, neither a workaround:
4. VIEWPORT AND SCISSOR were host-fixed to the full target. They are now the guest's own per draw (draw_util::GetHostViewportInfo/GetScissor, the same call DeriveSystemConstants already made for NDC scale/offset) as Vulkan dynamic state. Frame 600 carries 13 distinct viewport/scissor combinations -- 48 draws scissored to 16x16, 4 draws with a 1280x208 viewport; the fixed viewport was wrong for 77 of 171 draws.
5. kQuadList (0x0D) fell through to VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, which regroups the vertices into unrelated triangles. It is now expanded to a triangle list (0,1,2 / 0,2,3 per group of 4) as Xenia's PrimitiveProcessor does. This frame's ENTIRE world geometry is quad_list. Measured effect: the full-screen multiply passes now cover the whole target instead of one triangle of it -- which is exactly why the frame changed from 'left at the clear colour' to 'uniformly black'. kRectangleList (19 draws) is still NOT converted; it needs a synthesized 4th vertex, not just an index remap.

NEXT STEP: find why draws 32+ contribute no pixels. Ruled out already: textures, depth test, depth clear value, viewport/scissor. Candidates not yet tested: kRectangleList conversion, cull mode (PA_SU_SC_MODE_CNTL is still host-fixed to NONE), the vertex fetch reading past the 64 MiB SSBO mirror (guestPhysicalMirrorBytes) for world vertex buffers, and missing mip tails making the full-screen passes sample level 0 under heavy minification.

### Reopened (2026-07-23)
REOPENED deliberately: the SYMPTOM in the title (the scene frame is black) is NOT fixed. Only the diagnosis was -- and it was falsified. Stub textures were necessary but not sufficient; 80.7% of bindings now carry verified real guest data and the frame is still 0 of 921600 px non-black. Keep this entry open until a scene frame shows guest imagery; the resolution note above is the current state of the investigation, not a fix.

### Resolution (2026-07-23)
RESOLVED. The scene frame renders the game: 919796-921600 of 921600 px non-black (99.8%-100.0%) over 4 runs of the fixed build, against 0 of 921600 in every one of 9 runs before it. scratch/screenshots/fixed/frame.png is the real Gears of War title screen -- logo, PRESS START, the Epic copyright line, and the wall art that tools/decode_bc.py had already decoded from the texture dump.

ROOT CAUSE: the SHADER MODIFICATION, not anything in the output merger, the textures, the geometry or the render target.

A Xenos shader's SPIR-V translation is not a function of its microcode alone. Xenia's SpirvShaderTranslator::Modification carries the INTERPOLATOR MASK the vertex and pixel shader exchange, and that mask is a property of the PAIR plus the draw's own SQ_PROGRAM_CNTL / SQ_CONTEXT_MISC registers -- vertex_shader->writes_interpolators() & pixel_shader->GetInterpolatorInputMask(...). Both stages were being translated with GetDefaultVertexShaderModification / GetDefaultPixelShaderModification, and those leave interpolator_mask = 0. So every vertex shader exported NO interpolators and every pixel shader read them as zero.

Why it hid so well: oPos is a builtin, not an interpolator, so position, clipping, culling and rasterisation were completely unaffected. GEARS_DRAW_STATS=1 (new: per-draw VK_QUERY_TYPE_PIPELINE_STATISTICS) measured 149 of 170 draws running the fragment shader, some with 1.8M invocations on a 921600-pixel target -- every one shading pure black. The only draws that ever produced colour were the few whose oC0 comes from float constants alone (ps_b49ec2b161f2352e: `mul oC0.xyz, c0.xyz, c255.x`), which is exactly the 7910 px in the top-left this entry recorded. Every pixel shader in the frame's world geometry ends in a multiply by an interpolant (ps_e435892336fbffc8: `mul r0.xyz, r0.zyx, r4.wwww` then `mul oC0.xyz, r0.zyx, c255.x`), so r4 = 0 made them black regardless of texture, blend, depth or viewport.

THE FIX: DeriveShaderModifications (runtime/gpu_draw_xlate.cpp) ports VulkanPipelineCache::GetCurrentVertexShaderModification / GetCurrentPixelShaderModification in full -- interpolator mask, centroid mask (GetInterpolatorSamplingPattern over RB_SURFACE_INFO msaa_samples / SQ_CONTEXT_MISC sc_sample_cntl / SQ_INTERPOLATOR_CNTL), dynamic addressable register counts from vs_num_reg/ps_num_reg (they were passed as 0), user clip planes from PA_CL_CLIP_CNTL, point parameters, param-gen, the depth/stencil early-Z hint, and the MIN/MAX blend pre-multiply factors. It runs per draw, before either stage is translated. TranslateShader now takes the modification, and the renderer's shader/module cache is keyed by (microcode hash, modification) rather than hash alone -- frame 600 holds 34 translations of 26 distinct microcodes as a result. One analysed Shader object is cached per microcode (GetAnalyzedShader), as Xenia does, because ucode analysis is what answers the interpolator question.

RULED OUT BY CONTROLLED ARMS, none of them the cause:
- The 64 MiB guestPhysicalMirrorBytes SSBO bound, which was the leading suspect. New 'frame geometry reach' census (from the vertex bindings the shader itself declares, now carried out of the translator): 0 of 170 draws fetch vertices past the mirror; highest vertex-buffer end 0xc3f780 = 12.8 MiB. The mirror was never the limit and must not be bumped.
- Blending. GEARS_DRAW_NOBLEND=1 (new arm) writes the shader's own output straight to the target: still 0 non-black.
- Texture content. GEARS_DRAW_NOTEX=1 white stubs: still 0 non-black.
- Vertex data. GEARS_DRAW_VDUMP=N (new arm) dumped draw 32's vertices at the shader's own 16-dword stride: real world positions, per-corner selectors and a (0.808, 0.623, 0.960, 0.912) colour float4. The data was always fine.
- Float constants. Per-draw census of the packed VS/PS constant UBOs: c255.x = 8.0 on the world draws, not 0.

DIAGNOSTICS DEFECT FIXED IN PASSING: the frame report's single coverage number counted pixels != the clear colour, so a uniformly BLACK frame scored 100%. It now reports 'px non-black' and 'px changed from the clear' separately.

WHAT REMAINS (new entry territory, not this one): the frame is tinted pink/red -- a colour path is still wrong, undiagnosed -- and a hard diagonal split runs across it, which is kRectangleList still falling through to a triangle list instead of getting its synthesized 4th vertex, so half of every rectangle is missing.

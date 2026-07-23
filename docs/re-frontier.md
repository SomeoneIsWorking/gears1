# RE Frontier — the ordered RE dependency chain toward a faithful BL2

Tracked by `tools/re_frontier.py` (consult it FIRST; update it in the SAME commit
that changes a step). This is the fine-grained companion to `docs/codemap.md`:
the codemap says *what subsystem exists*, this says *which ordered RE step is
real reverse-engineering vs a hack that jumped ahead*.

**Hard rule (no hacks / no fallbacks):** a `⛔ hack` status is DEBT, never an
acceptable resting state. It marks a shortcut standing in for absent RE and MUST
be removed as its real mechanism lands. `re_frontier.py hacks` is the debt list;
`re_frontier.py next` tells you the next RE-ready step.

**`re-verified` MEANS FAITHFUL to the real target — not "the mechanism runs."** A
step is `re-verified` only when its OUTPUT matches the real game/binary (look /
sound / behavior) on real data. An internal trace ("bytecode reached the call
site", "N rows attached") is a mechanism check, NOT faithfulness — if it runs but
the result doesn't match the real target, it is `re-partial` with the
faithfulness gap named. The user observes the running system; that observation
overrides any internal trace.

**Fail fast & loud:** a failure must surface loudly, never silently fall back —
unless the fallback IS intended behavior of the real target being reproduced.

Statuses: ✅ re-verified · 🟡 re-partial (honest gap) · 🔬 in-progress ·
⛔ hack (debt, must remove) · ⬜ todo · ➖ skip-by-design · ⏸ blocked (computed).

<!-- Machine-edited by tools/re_frontier.py add/set. Format: `## <area>` sections;
     each entry is `### <id> — <title>` followed by `- <field>: <value>` lines. -->

## core

### area.first-step — Describe the first RE step in this chain
- status: todo
- deps: 
- evidence: 
- where: 
- gap: Fill in real steps; add deps to encode the RE dependency order.
- notes: 


## gpu

### cmd-processor — PM4 command processor executes the ring
- status: re-verified
- deps: 
- evidence: runtime/vd_null_gpu.cpp; executes TYPE0/IB/DRAW_INDX/fences/predication/IM_LOAD; verified 30fps 20000+ frames
- where: runtime/vd_null_gpu.cpp
- gap: 
- notes: 

### shader-xlate — Xenos microcode -> SPIR-V translation
- status: re-verified
- deps: cmd-processor
- evidence: xenia_gpu/ island; 38/38 runtime-bound shaders pass spirv-val, reverified independently
- where: xenia_gpu/, tools/xenos_translate
- gap: 
- notes: 

### shader-capture — Capture runtime-bound shaders at IM_LOAD
- status: re-verified
- deps: cmd-processor
- evidence: catalog #21; 38 distinct payloads, stable across runs, captured at PM4 IM_LOAD
- where: runtime/vd_null_gpu.cpp
- gap: 
- notes: 

### present — Host swapchain present driven by guest VdSwap
- status: re-verified
- deps: cmd-processor
- evidence: runtime/gpu_present.cpp; present 1:1 with guest swap over 8190 frames; headless falls back
- where: runtime/gpu_present.cpp
- gap: 
- notes: 

### const-capture — Track SET_CONSTANT: ALU-float/fetch/bool-loop constant files
- status: re-verified
- deps: cmd-processor
- evidence: catalog #23; at the hot-pair DRAW_INDX the ALU float file decodes to a real transform (c0-c3 identity + c4/c5 -1.209 screen scale, matching the hot VS's own c0-c3 usage) and the fetch file to 3 well-formed textures (1280x720 tiled RT + two 640x360); reproducible across runs. Bases (0x4000/0x4800/0x4900/0x4908) match Xenia register_table.inc + command_processor.cc.
- where: runtime/vd_null_gpu.cpp (TrackConstantLoad, DumpConstantFiles); GEARS_CONST_DUMP=1
- gap: 
- notes: This title feeds ALU-float + fetch constants via plain TYPE0 register writes (already handled), NOT ring SET_CONSTANT type-0/1 (0 over 950 frames). What WAS being dropped and is now handled: LOAD_ALU_CONSTANT 0x2F (type-0 ALU-from-memory, ~12/hot-pair-draw) and SET_CONSTANT 0x2D type-4 REGISTERS (~38/draw); SET_CONSTANT2/SET_SHADER_CONSTANTS never fire. The hot VS's vertex fetch constant IS present in the fetch file at draw time (draw-params RE-VERIFIED this): fetch constant #95 at reg 0x48BE = 0x00097813 (type 3, vertex base 0x97810). The earlier "0 type-3 constants" was a scanner artifact -- the slot-based dump only scans j-entries of slots whose leading dword is type 3, and #95 is at j=2 of slot 31 (leading dword #93 not type 3). The vfetch base was in the register file all along; catalog #22's instruction patch supplies the stride+const-index, not the base.

### draw-params — Detect hot pair at DRAW_INDX and capture draw params
- status: re-verified
- deps: shader-capture, const-capture
- evidence: Captured a representative hot-pair (vs_5363d074/ps_501ac5d8) DRAW_INDX on a real run (GEARS_DRAW_CAPTURE=1, scratch/draw-params/hot_draw.txt). Packet decoded per Xenia (pm4_command_processor_implement.h ExecutePacketType3Draw + registers.h VGT_DRAW_INITIATOR/VGT_DMA_SIZE): VGT_DRAW_INITIATOR=0x00060804 -> prim_type=triangle_list, source_select=kDMA(indexed), index_size=int32, num_indices=6; VGT_DMA_BASE=0x978d0, VGT_DMA_SIZE num_words=12 swap_mode=2(k8in32). GEOMETRY SOURCE RECOVERED from the fetch register file: the hot VS's vfetch reads vertex fetch constant #95 (Xenia disasm "vf0" == 95-storage_index; spirv-dis confirms fetch_constants[0][47][2..3] = fetch-file dwords 190,191 = reg 0x48BE/0x48BF), which at draw time = 0x00097813 0x100000c2 -> type 3 (vertex), base 0x97810, size 192 bytes; baked Stride=12 dwords (48 bytes). COHERENCE (all cross-consistent): indices 0 1 3 0 3 2 = two triangles of a quad, max index 3; vertex buffer size 192 bytes = exactly 4 vertices (indices 0..3 in range); vertices are a clean NDC full-screen quad (-1,1)(1,1)(-1,-1)(1,-1), z=0 w=1; vertex buffer 0x97810..0x978d0 abuts the index buffer at 0x978d0 (contiguous). This is the full-screen RT-sampling pass predicted by system-constants.
- where: runtime/vd_null_gpu.cpp CaptureHotDraw + ReadGuest32Raw (GEARS_DRAW_CAPTURE=1, GEARS_DRAW_CAPTURE_ANY drops hot-pair filter, GEARS_DRAW_CAPTURE_DIR); report scratch/draw-params/hot_draw.txt
- gap: none for the hot pair. Minor open detail: VGT_DMA_SIZE.num_words=12 gives Xenia length 48 bytes vs 24 for 6 int32 indices; num_indices=6 is the authoritative draw count either way. Immediate-index (kImmediate) and kAutoIndex paths not exercised by this draw (both handled/reported by the capture if they occur).
- notes: CORRECTION to const-capture/#23: the fetch file DOES hold the vertex fetch constant at draw time (#95, type 3) -- the earlier "0 type-3 constants" was a scanner artifact (the slot-based dump only entered its j-loop for slots whose LEADING dword was type 3; #95 sits at j=2 of slot 31, whose leading dword #93 is not type 3, so it was skipped). The vfetch base was in the register file all along; the catalog #22 instruction patch supplies stride+const-index, not the base.

### system-constants — Port Xenia SystemConstants + UpdateSystemConstantValues
- status: re-verified
- deps: const-capture
- evidence: tools/system_constants produces the 528-byte xe_uniform_system_constants UBO from a real hot-pair register snapshot (scratch/bin/regfile_hotpair.bin; scratch/bin/system_constants.bin + scratch/logs/system_constants.txt). Layout verified byte-exact against the bound shader's SPIR-V: every OpMemberDecorate Offset of %XeSystemConstants in vs_5363d0746b3ef666.spv matches the C++ struct (flags@0, ndc_scale@16, ndc_offset@32, texture_swizzled_signs@64, ..., texture_integer_scale_bits@400, size 528). Field values consistent with a 1280x720 D3D target: ndc_scale=(1,-1,1) [Y-flip from YSCALE=-360], ndc_offset=(1/1280,1/720,0) [D3D9 half-pixel], vertex_base_index=0, color_exp_bias=(1,1,1,1), zpd_fsi_counter_index=UINT32_MAX. flags=0x3c20 (WNotReciprocal | DepthFloat24 | alpha=kAlways) from the register snapshot; with the draw-params-measured VGT_DRAW_INITIATOR=0x00060804 (prim_type=triangle_list) injected, flags=0x3c60 (+PrimitivePolygonal) -- scratch/bin/system_constants_complete.bin, the definitive value.
- where: tools/system_constants/main.cpp (derivation, ports UpdateSystemConstantValues non-FSI path, reuses Xenia draw_util::GetHostViewportInfo); runtime/vd_null_gpu.cpp DumpConstantFiles (register-file snapshot, GEARS_CONST_DUMP). Optional at configure time (GEARS_HAVE_XENOS_TRANSLATOR), links xenia_gpu, headless-safe.
- gap: none for the register-derived / geometry fields. STUBBED (visibly, reported by the tool): texture_swizzled_signs / texture_swizzles / textures_resolved / texture_integer_scale_bits (texture-cache state, not geometry); vertex_index_load_address / vertex_index_endian + PrimitivePolygonal/PrimitiveLine flag bits (draw-parameter state -- VGT_DRAW_INITIATOR/DRAW_INDX not yet mirrored into our register file; draw-params owns it). All edram_*/FSI fields correctly zero on the non-FSI host-render-targets path.
- notes: ndc_scale[0]=0.99999994 (not exactly 1) is Xenia's ArchReciprocalRefined, reproduced faithfully. VGT_DRAW_INITIATOR is a register (0x21FC) the tool already reads, but our command processor does not mirror the DRAW_INDX packet into it (draw-params owns DRAW_INDX). draw-params measured 0x00060804 (triangle_list) for this same draw; pass `--draw-initiator=0x00060804` to inject it -> flags 0x3c20 becomes 0x3c60 (+PrimitivePolygonal). The clean permanent fix is for the DRAW_INDX handler to mirror VGT_DRAW_INITIATOR into the register file; that belongs to draw-params, not here.

### draw-backend — First real draw: hot pair geometry into swapchain
- status: re-partial
- deps: shader-xlate, present, draw-params, system-constants
- evidence: runtime/gpu_draw.cpp renders the hot pair (vs_5363d074/ps_501ac5d8) into a 1280x720 offscreen target headless (GEARS_DRAW=1) and reads it back to scratch/screenshots/hot_draw.ppm. PROVEN on a real run: (1) runtime translation reproduces the verified SPIR-V byte-for-byte (VS 12420, PS 18588 bytes); (2) the four vertices read from the guest shared-memory SSBO at physical 0x97810 are the real full-screen NDC quad (-1.0007813,1.0013889)(0.9992187,1.0013889)(-1.0007813,-0.9986111)(0.9992187,-0.9986111) z=0 w=1 (half-texel offset for a 1280 RT); (3) with the render target cleared to a magenta sentinel, all 921600 px are overwritten by the draw -> the full-screen quad rasterised (triangle_list, 6 int32 indices 0 1 3 0 3 2 from guest 0x978d0) and the pixel shader ran over every pixel; (4) Vulkan validation is clean. Output is BLACK, not a console-matching frame -> re-partial not re-verified.
- where: runtime/gpu_draw.cpp (Vulkan renderer: SSBO mirror of guest phys mem, system/float(packed per Xenia constant map)/bool-loop/fetch UBOs, 1x1 stub texture0+sampler, pipeline from the 2 runtime-translated SPIR-V modules), runtime/gpu_draw_xlate.cpp (Xenos->SPIR-V + system-constants derivation, isolated from system Vulkan headers), runtime/vd_null_gpu.cpp (VGT_DRAW_INITIATOR mirrored into the register file at DRAW_INDX; TriggerHotDraw fires the backend once), runtime/gpu_present.cpp (uploads the rendered frame into the swapchain). GEARS_DRAW_VALIDATE=1 enables Vulkan validation.
- gap: HONEST GAP (why re-partial): this hot pair is a full-screen RT-sampling post-process (samples a 1280x720 render target, catalog #23). texture0 is a 1x1 stub, so the sample returns 0 and the pixel shader's log()/exp() path collapses the frame to black -- verified this is the sole cause (identity-swizzle diagnostic still black -> the stub sample itself is 0, not a pipeline bug; VS position is correct so SSBO/UBO feeding all work). Recognisable output needs the RT it samples to be produced first.
- notes: NEXT RE STEP (see draw-backend-rt below): execute the upstream draws that render the scene into the RT this pair samples, OR target a self-contained hot pair that emits its own geometry+shading, so a console-matching frame can be verified. The pipeline, constant feeding, geometry fetch and present path are all in place and proven; only the sampled RT content is missing.

### draw-backend-rt — Produce the render target the hot pair samples
- status: re-partial
- deps: draw-backend
- evidence: The RT link is IMPLEMENTED and measured. Mechanism, all from the guest's own registers with no EDRAM model: the frame's per-draw register snapshots are scanned for RB_COPY_DEST_BASE (0x2319), the main-memory addresses the guest resolves EDRAM to; a texture binding whose fetch constant (0x4800 + fc*6, dword1 bits 12..31 << 12) names one of those addresses is bound to the rendered colour target instead of the stub. Because Vulkan forbids sampling the bound attachment, the frame's single render pass is SPLIT at each such draw: the pass ends, colour is copied into a separate sampled image (rtSample), and the pass resumes with LOAD so prior draws are preserved. MEASURED on scene frame 600 with GEARS_DRAW_RT=1: 8 distinct resolve destinations found (0x30c000 0x6e0000 0xba40000 0xbcc0000 0xbde0000 0xc2e0000 0xc7e9000 0xcb81000); 30 of 224 texture bindings (13.4%) resolved to the rendered RT, 194 still stub; 10 render-pass segments, 9 RT snapshots; 176/176 draws issued. The hot pair (vs_5363d074/ps_501ac5d8) is draw 30 of the frame and its fetch constant 0 base is 0xbde0000, which IS one of the resolve destinations -- catalog #23's address confirmed live in a scene frame.
- where: runtime/gpu_draw.cpp (Renderer::RenderFrameImpl: resolveDests scan of RB_COPY_DEST_BASE, selectTexView fetch-constant base matching, rtSample image + copyColorToImage, renderPassLoad + segmented draw loop), GEARS_DRAW_RT=1 enables it.
- gap: Still lights up nothing, and the reason is still upstream: the accumulated colour target it binds is itself black. Texture upload -- the thing this step was waiting on -- is now DONE (draw-backend-textures) and did not change that; see draw-backend-frame's gap for what does. Measured unchanged with real textures: 30 of 216-218 texture bindings served by the rendered RT. The two fidelity gaps in the link itself are also unchanged and still unobservable: (1) all 8 resolve destinations are conflated onto ONE host colour target, which needs a per-surface model keyed on RB_COLOR_INFO base + RB_SURFACE_INFO pitch with resolve EVENTS captured, not just draws; (2) no tiling, format conversion or scaling is applied on the resolve.
- notes: This step was the presumed blocker for draw-backend-frame; measuring it falsified that. The direct binding does NOT need the full EDRAM resolve model to be wired -- RB_COPY_DEST_BASE plus fetch-constant base matching is enough to identify which draws sample the frame's own RT, and the render-pass split is enough to feed them. What it needs to be USEFUL is real texture data, so the frame it samples is not black. Ordering: texture upload first, then per-surface resolve targets.

### draw-backend-frame — Whole-frame backend: every draw of a frame into one persistent target
- status: re-partial
- deps: draw-backend
- evidence: runtime/gpu_draw.cpp RenderFrame() renders every DRAW_INDX/_2 of ANY selected frame: per-draw register-file snapshot, per-shader Xenos->SPIR-V translation cached by hash, per-shader TEXTURE DESCRIPTOR SET LAYOUTS derived from the translated shader's own binding list, pipelines cached per (vs,ps,prim,output-merger state), per-draw UBOs, all draws in SUBMISSION ORDER into one persistent colour+depth target, readback to PPM. MEASURED headless on a real POST-LOAD SCENE frame (GEARS_DRAW_FRAME_AT=600): 173-176 of 173-176 draws issued, 0 skipped, 22 distinct shader pairs, 26 distinct shaders, 28 pipelines, 6 texture layouts, 5 pipeline layouts, 224 texture bindings; primitives point_listx48 triangle_listx93 rectangle_listx19 quad_listx16. Real guest geometry rasterises from the guest's own memory (checkpoint scratch/screenshots/cp30.png shows polygons at draw 30 over the clear). Draws-per-frame profile measured across 4300 frames: loading frames carry 2-3 draws, the scene phase starts at frame 571 and holds 168-186 draws/frame.
- where: runtime/gpu_draw.h (FrameDrawItem/FrameDrawInputs/RenderFrame), runtime/gpu_draw.cpp (Renderer::RenderFrameImpl: shader+pipeline caches, per-draw UBOs/descriptor sets, persistent colour+depth RT, one render pass, readback+PPM), runtime/gpu_draw_xlate.cpp (TranslateShader: single-stage translation for per-hash caching), runtime/vd_null_gpu.cpp (CaptureFrameDraw accumulates per-draw snapshots; TriggerFrameRender fires at the swap; GEARS_DRAW_FRAME_AT=N selects which frame; GEARS_DRAW_CENSUS=1 reports the per-run draw census).
- gap: Output is STILL BLACK (0 of 921600 px non-black), but the cause has moved and texture upload is no longer it -- draw-backend-textures is done and verified, and both arms of a controlled A/B (GEARS_DRAW_NOTEX=1 vs real textures, plus GEARS_DRAW_FIXEDVP=1 vs guest viewport, 3 arms measured) give exactly 0 non-black pixels. What the per-draw checkpoints now show: the scene geometry of draws 1-28 covers only 7910 of 921600 px (0.86%) and sits in the top-left corner; draws 29-31 are full-screen quad_list passes with RB_BLENDCONTROL0 = 0x1000400, which is src*ZERO + dst*SRC_COLOR -- a pure multiply -- sampling the guest's own 256x32 k_DXT1 texture at 0x1f45000, which is genuinely near-black, so the whole target goes to (0,0,0); and every draw from 32 to 170, which is where this frame's actual world geometry lives (quad_list, 10-texture shaders, additive and alpha blending), adds exactly ZERO pixels on top of that black. Depth is ruled out as the cause: GEARS_DRAW_DEPTH_CLEAR=0.0 vs 1.0 and GEARS_DRAW_NODEPTH=1 all give 0 non-black. So the world draws are not rasterising, or are shading zero, for a reason upstream of the output-merger -- that is the next thing to find. Cull mode (PA_SU_SC_MODE_CNTL) is still host-fixed to NONE. Mip tails are not uploaded, so the full-screen passes sample level 0 under heavy minification.
- notes: Milestone steps 1, 2, 3 and texture upload are all IMPLEMENTED. Two more per-draw states became guest-derived while isolating the black frame, and both are correctness fixes with the hardware contract behind them, not tuning: (d) VIEWPORT AND SCISSOR are now the guest's own, per draw, via Xenia draw_util::GetHostViewportInfo/GetScissor (the same call DeriveSystemConstants already made for NDC scale/offset, so the two cannot disagree) and are set as Vulkan dynamic state. Measured on frame 600: 13 distinct viewport/scissor combinations, including 48 draws scissored to 16x16 and 4 draws to a 1280x208 viewport -- a host-fixed full-target viewport was wrong for 77 of 171 draws. (e) kQuadList (0x0D) is EXPANDED to a triangle list (0,1,2 / 0,2,3 per group of 4), which is what Xenia's PrimitiveProcessor does; it previously fell through to VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, which regroups the vertices into unrelated triangles. This frame's entire world geometry is quad_list, so it was drawing garbage. Measured effect: the frame's full-screen multiply passes now cover the whole target instead of one triangle of it -- which is why the frame went from 'left at the clear colour' to 'uniformly black'. kRectangleList (19 draws) is still NOT converted (it needs a synthesized 4th vertex, not just an index remap) and still falls through to a triangle list. New diagnostics: GEARS_DRAW_NOTEX=1, GEARS_DRAW_FIXEDVP=1, GEARS_DRAW_NODEPTH=1, GEARS_DRAW_DEPTH_CLEAR=<float>, GEARS_DRAW_TEX_DUMP=1 -- all control arms, none of them a fix.

### draw-backend-textures — Upload the guest's own textures from its fetch constants
- status: re-verified
- deps: draw-backend-frame
- evidence: Texture fetch constants (0x4800 + fc*6) are decoded into host images with Xenia's own machinery: texture_util::GetSubresourcesFromFetchConstant (extents, mip range, base/mip pages), texture_util::GetGuestTextureLayout (row pitch, z-slice and array-slice strides), texture_address::Tiled2D/Tiled3D (the tiled address function) and FormatInfo (block size, bytes per block). Endianness is applied as an XOR on the source byte offset (k8in16 -> ^1, k8in32 -> ^3, k16in32 -> ^2), which is Xenia's XeEndianSwap expressed per byte. The guest swizzle is composed with the host format's own component order exactly as TextureCache::GuestToHostSwizzle does, and applied through VkImageView component mapping. MEASURED on scene frame 600 (GEARS_DRAW_FRAME_AT=600, GEARS_DRAW_RT=1): 26 distinct texture fetch constants, 20 uploaded, 176 of 218 texture bindings (80.7%) served by real guest data, 30 by the rendered RT, 12 by a stub. Format distribution across the frame's fetches: k_DXT4_5 x13, k_DXT1 x7, k_24_8_FLOAT x2, k_16_16 x1, k_16_16_16_16_EXPAND x1, k_16_16_16_16_FLOAT x1, k_8_8_8_8 x1 -- all 2D, all tiled, endian k8in16, swizzle 0x688 (identity RGBA). DXT1+DXT4_5 alone are 20 of 26. DECODE VERIFIED INDEPENDENTLY: GEARS_DRAW_TEX_DUMP=1 writes the decoded blob per texture and tools/decode_bc.py turns it into a PNG; scratch/screenshots/texdump/01caf000_k_DXT4_5_1024x512x1_tiled.png is unmistakable Gears world art (a grimy concrete/steel wall) and 02eb1000_k_DXT1_256x256x1_tiled.png is a clean radial falloff map. Detiling, endian and block decode are therefore correct on real data, not merely plausible. Sampler state is the guest's too: filters, clamp modes and anisotropy come from the fetch constant each shader sampler binding names (texture_util::GetClampModesForDimension plus the kUseFetchConst fallbacks), 7 distinct samplers on this frame. Upload cost: 1.5 MiB, RenderFrame 373 ms (was ~520 ms with stubs).
- where: runtime/gpu_draw_xlate.h/.cpp (DecodeGuestTexture, DeriveSamplerState, TexHostFormat/GuestTexture/GuestSamplerState -- the plain-type bridge, because Xenia's bundled Vulkan-Headers cannot share a TU with the system ones), runtime/gpu_draw.cpp (uploadTexture: host VkFormat table mirroring Xenia's vulkan_texture_cache host format table, image+view+staging creation, the census; getSampler; selectTexView tries the guest texture before the stub; the copy-buffer-to-image pass before the render pass). Knobs: GEARS_DRAW_NOTEX=1 control arm (stubs only), GEARS_DRAW_TEX_DUMP=1 decoded-blob dump.
- gap: Only the BASE level is uploaded -- mip tails are not read, so a minified sample reads the full-resolution level. 6 of 26 distinct fetches are not uploaded and are counted with their reason: 6 'not a texture fetch constant' (a shader declares a texture binding whose fetch slot the draw left unprogrammed). k_24_8_FLOAT (tiled depth, 2 fetches) and k_16_16_16_16_EXPAND have no host format mapping in the table yet. Half-way clamp modes (kClampToHalfway/kMirrorClampToHalfway) fall back to the nearest edge mode -- no host equivalent exists. No texture cache across frames: every frame re-decodes and re-uploads.
- notes: This step is DONE and does not explain the black frame -- that was the reason to do it, and doing it falsified it. See draw-backend-frame's gap for what actually blocks the picture now.


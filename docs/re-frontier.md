# RE Frontier — the ordered RE dependency chain toward GearsUE3

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
- evidence: runtime/gpu_draw.cpp renders the hot pair (vs_5363d074/ps_501ac5d8) into a 1280x720 offscreen target headless and reads it back to a PPM. PROVEN on a real run: (1) runtime translation reproduces the verified SPIR-V byte-for-byte (VS 12420, PS 18588 bytes); (2) the four vertices read from the guest shared-memory SSBO at physical 0x97810 are the real full-screen NDC quad (-1.0007813,1.0013889)(0.9992187,1.0013889)(-1.0007813,-0.9986111)(0.9992187,-0.9986111) z=0 w=1 (half-texel offset for a 1280 RT); (3) with the render target cleared to a magenta sentinel, all 921600 px are overwritten by the draw -> the full-screen quad rasterised (triangle_list, 6 int32 indices 0 1 3 0 3 2 from guest 0x978d0) and the pixel shader ran over every pixel; (4) Vulkan validation is clean. Output is BLACK, not a console-matching frame -> re-partial not re-verified.
- where: runtime/gpu_draw.cpp (Vulkan renderer: SSBO mirror of guest phys mem, system/float(packed per Xenia constant map)/bool-loop/fetch UBOs, 1x1 stub texture0+sampler, pipeline from the 2 runtime-translated SPIR-V modules), runtime/gpu_draw_xlate.cpp (Xenos->SPIR-V + system-constants derivation, isolated from system Vulkan headers), runtime/vd_null_gpu.cpp (VGT_DRAW_INITIATOR mirrored into the register file at DRAW_INDX), runtime/gpu_present.cpp (presents the rendered frame). GEARS_DRAW_VALIDATE=1 enables Vulkan validation. REPRODUCTION MOVED (2026-08-04): the one-shot hot-pair renderer this evidence was taken with (GEARS_DRAW=1, TriggerHotDraw/RenderHotDraw) has been deleted -- it was a second renderer for one known shader pair, drifting untested beside the one that ships. The same draw now goes through the whole-frame backend (draw-backend-frame), which subsumes it; re-run it there with GEARS_DRAW_FRAME_AT/_COUNT and read the per-draw diag table. The findings above are unchanged, only the harness that produced them is gone.
- gap: HONEST GAP (why re-partial): this hot pair is a full-screen RT-sampling post-process (samples a 1280x720 render target, catalog #23). texture0 is a 1x1 stub, so the sample returns 0 and the pixel shader's log()/exp() path collapses the frame to black -- verified this is the sole cause (identity-swizzle diagnostic still black -> the stub sample itself is 0, not a pipeline bug; VS position is correct so SSBO/UBO feeding all work). Recognisable output needs the RT it samples to be produced first.
- notes: NEXT RE STEP (see draw-backend-rt below): execute the upstream draws that render the scene into the RT this pair samples, OR target a self-contained hot pair that emits its own geometry+shading, so a console-matching frame can be verified. The pipeline, constant feeding, geometry fetch and present path are all in place and proven; only the sampled RT content is missing.

### draw-backend-rt — Produce the render target the hot pair samples
- status: re-partial
- deps: draw-backend
- evidence: The RT link is IMPLEMENTED and measured. Mechanism, all from the guest's own registers with no EDRAM model: the frame's per-draw register snapshots are scanned for RB_COPY_DEST_BASE (0x2319), the main-memory addresses the guest resolves EDRAM to; a texture binding whose fetch constant (0x4800 + fc*6, dword1 bits 12..31 << 12) names one of those addresses is bound to the rendered colour target instead of the stub. Because Vulkan forbids sampling the bound attachment, the frame's single render pass is SPLIT at each such draw: the pass ends, colour is copied into a separate sampled image (rtSample), and the pass resumes with LOAD so prior draws are preserved. MEASURED on scene frame 600 with : 8 distinct resolve destinations found (0x30c000 0x6e0000 0xba40000 0xbcc0000 0xbde0000 0xc2e0000 0xc7e9000 0xcb81000); 30 of 224 texture bindings (13.4%) resolved to the rendered RT, 194 still stub; 10 render-pass segments, 9 RT snapshots; 176/176 draws issued. The hot pair (vs_5363d074/ps_501ac5d8) is draw 30 of the frame and its fetch constant 0 base is 0xbde0000, which IS one of the resolve destinations -- catalog #23's address confirmed live in a scene frame.
- where: runtime/gpu_draw.cpp (Renderer::RenderFrameImpl: resolveDests scan of RB_COPY_DEST_BASE, selectTexView fetch-constant base matching, rtSample image + copyColorToImage, renderPassLoad + segmented draw loop), the link is on by default (GEARS_DRAW_NORT=1 disables it).
- gap: It now feeds a target that is no longer black (draw-backend-frame's shader-modification fix), so the link is finally observable. Texture upload -- the thing this step was waiting on -- is now DONE (draw-backend-textures) and did not change that; see draw-backend-frame's gap for what does. Measured unchanged with real textures: 30 of 216-218 texture bindings served by the rendered RT. The two fidelity gaps in the link itself are also unchanged and still unobservable: (1) all 8 resolve destinations are conflated onto ONE host colour target, which needs a per-surface model keyed on RB_COLOR_INFO base + RB_SURFACE_INFO pitch with resolve EVENTS captured, not just draws; (2) no tiling, format conversion or scaling is applied on the resolve.
- notes: This step was the presumed blocker for draw-backend-frame; measuring it falsified that. The direct binding does NOT need the full EDRAM resolve model to be wired -- RB_COPY_DEST_BASE plus fetch-constant base matching is enough to identify which draws sample the frame's own RT, and the render-pass split is enough to feed them. What it needs to be USEFUL is real texture data, so the frame it samples is not black. Ordering: texture upload first, then per-surface resolve targets.

### draw-backend-frame — Whole-frame backend: every draw of a frame into one persistent target
- status: re-verified
- deps: draw-backend
- evidence: runtime/gpu_draw.cpp RenderFrame() renders every DRAW_INDX/_2 of ANY selected frame: per-draw register-file snapshot, per-draw SHADER MODIFICATION derivation, Xenos->SPIR-V translation cached per (microcode hash, modification), per-shader TEXTURE DESCRIPTOR SET LAYOUTS derived from the translated shader's own binding list, pipelines cached per (vs,ps,prim,output-merger state), per-draw UBOs, all draws in SUBMISSION ORDER into one persistent colour+depth target, readback to PPM. THE FRAME NOW SHOWS THE GAME. MEASURED headless on scene frame 600 (GEARS_DRAW_FRAME_AT=600, ), 4 runs: 169-171 of 169-171 draws issued, 0 skipped, and 919796-921600 of 921600 pixels non-black (99.8%-100.0%) -- against 0 of 921600 non-black in every one of 9 runs before the fix. scratch/screenshots/fixed/frame.png is the real Gears of War title screen: the logo, 'PRESS START', the Epic copyright line and the grimy wall texture all legible. 22 distinct shader pairs, 34 distinct (shader, modification) translations from 26 distinct microcodes, 28 pipelines, 6 texture layouts, 5 pipeline layouts, 214 texture bindings (172 real guest textures, 30 from the rendered RT, 12 stub). Draws-per-frame profile measured across 4300 frames: loading frames carry 2-3 draws, the scene phase starts at frame 571 and holds 168-186 draws/frame.
- where: runtime/gpu_draw.h (FrameDrawItem/FrameDrawInputs/RenderFrame), runtime/gpu_draw.cpp (Renderer::RenderFrameImpl: per-draw DeriveShaderModifications call, shader cache keyed by (hash, modification), pipeline cache, per-draw UBOs/descriptor sets, persistent colour+depth RT, readback+PPM), runtime/gpu_draw_xlate.cpp (DeriveShaderModifications: the interpolator mask and the rest of Xenia's Modification, per draw; TranslateShader takes that modification; GetAnalyzedShader caches one analysed Shader per microcode), runtime/vd_null_gpu.cpp (CaptureFrameDraw accumulates per-draw snapshots; TriggerFrameRender fires at the swap; GEARS_DRAW_FRAME_AT=N selects which frame; GEARS_DRAW_CENSUS=1 reports the per-run draw census).
- gap: Historical early-frame snapshot; later steps below own current renderer gaps. Do not use this old list to infer present support.
- notes: THE BLACK FRAME IS FIXED, and the cause was the SHADER MODIFICATION, not anything downstream. A Xenos shader's SPIR-V translation is not a function of its microcode alone: Xenia's SpirvShaderTranslator::Modification carries the INTERPOLATOR MASK the vertex and pixel shader exchange, and that mask is a property of the PAIR plus the draw's own SQ_PROGRAM_CNTL / SQ_CONTEXT_MISC registers. Both stages were being translated with GetDefaultVertexShaderModification / GetDefaultPixelShaderModification, which leave interpolator_mask = 0 -- so every vertex shader exported NO interpolators and every pixel shader read them as zero. Position, clipping and rasterisation were unaffected (oPos is a builtin, not an interpolator), which is why the frame looked so healthy while showing nothing: pipeline statistics measured 149 of 170 draws running the fragment shader, millions of invocations, and every one of them shading pure black. The only draws that produced colour were the handful whose oC0 comes from float constants alone (ps_b49ec2b161f2352e: mul oC0.xyz, c0.xyz, c255.x) -- 7910 px. DeriveShaderModifications now ports VulkanPipelineCache::GetCurrentVertex/PixelShaderModification in full (interpolator mask, centroid mask, dynamic addressable register counts from vs_num_reg/ps_num_reg, user clip planes, point parameters, param-gen, depth/stencil early-Z hint, MIN/MAX blend pre-multiply factors) and the shader cache is keyed by (hash, modification), which is why one frame now holds 34 translations of 26 microcodes. RULED OUT ALONG THE WAY, each by a controlled arm, none of them the cause: the 64 MiB guestPhysicalMirrorBytes SSBO bound (new 'frame geometry reach' census: 0 of 170 draws fetch past it, highest vertex-buffer end 0xc3f780 = 12.8 MiB, so the mirror was never the limit); blending (GEARS_DRAW_NOBLEND=1 left the frame at 0 non-black); texture content (GEARS_DRAW_NOTEX=1 white stubs, same 0). Earlier per-draw states that became guest-derived and remain correct: viewport and scissor via draw_util::GetHostViewportInfo/GetScissor (13 distinct combinations on this frame), and kQuadList expanded to a triangle list (0,1,2 / 0,2,3) as Xenia's PrimitiveProcessor does. Diagnostics, all control arms and never fixes: GEARS_DRAW_STATS=1 (per-draw pipeline statistics: vertices, primitives in, primitives after clip+cull, fragment invocations -- this is what separated 'not rasterising' from 'shading black'), GEARS_DRAW_ONLY=N (emit only draw N over the clear), GEARS_DRAW_VDUMP=N (dump draw N's first vertices out of the mirror at the shader's own stride), GEARS_DRAW_NOBLEND=1, GEARS_DRAW_NOTEX=1, GEARS_DRAW_FIXEDVP=1, GEARS_DRAW_NODEPTH=1, GEARS_DRAW_DEPTH_CLEAR=<float>, GEARS_DRAW_TEX_DUMP=1. The frame report now separates 'px non-black' from 'px changed from the clear' -- the old single number counted a uniformly BLACK frame as 100% covered.

### draw-backend-textures — Upload the guest's own textures from its fetch constants
- status: re-verified
- deps: draw-backend-frame
- evidence: Texture fetch constants are decoded with Xenia's `GetSubresourcesFromFetchConstant`, `GetGuestTextureLayout`, `Tiled2D`/`Tiled3D` and `FormatInfo`. Endianness and guest/host component swizzles remain exact. The decoder now walks every declared level, selects the separate base or mip allocation, applies packed-tail block offsets, and uploads all levels into one Vulkan image. Cache invalidation hashes both disjoint guest spans. On the cached chapter-45 frame, the pass-preserving probe changed from a broad 0.500076..0.968719 mip-0 distribution to a uniform 0.968719, exactly the verified Gears 1 oracle; identity-matched caster coverage changed from 13,491/5,884 to 31,621/12,147 versus oracle 31,618/12,147 (`C063`, catalog #109). Synthetic positive and negative tests cover authored mip storage and bounds refusal; five materially different cached frames replay successfully.
- where: runtime/gpu_draw_texture_decode.h/.cpp (format/layout/full mip-chain decode), runtime/gpu_draw_xlate.cpp (sampler derivation), runtime/gpu_draw_textures.cpp (multi-level image/view/staging upload, cache), runtime/guest_texture_hash.cpp (disjoint base+mip hashing). Knobs: GEARS_DRAW_NOTEX=1 control arm, GEARS_DRAW_TEX_DUMP=1 decoded-blob dump.
- gap: k_24_8_FLOAT and k_16_16_16_16_EXPAND still have no host format mapping. Half-way clamp modes fall back to the nearest host edge mode because Vulkan has no exact equivalent.
- notes: This step is DONE. It did not by itself explain the black frame -- that was the reason to do it, and doing it falsified it -- but with the shader-modification defect fixed the uploaded textures are visibly the ones on screen (the title-screen wall art in scratch/screenshots/fixed/frame.png is the same blob tools/decode_bc.py rendered from the dump).

### draw-backend-primitives — Convert every guest primitive type Xenia's PrimitiveProcessor does
- status: re-verified
- deps: draw-backend-frame
- evidence: kQuadList is expanded to a triangle list (0,1,2 / 0,2,3 per group of 4) as Xenia's PrimitiveProcessor does; this frame's entire world geometry is quad_list. kRectangleList is expanded by a GEOMETRY SHADER ported from Xenia's VulkanPipelineCache::GetGeometryShader (kRectangleList branch): the guest gives three vertices and the hardware infers the fourth by mirroring one across the LONGEST EDGE, comparing the three squared edge lengths in screen X/Y, and every attribute (position, interpolators, clip distances) is mirrored the same way -- so it cannot be synthesized in the index buffer ahead of the vertex shader. Xenia's VS-expansion fallback (kRectangleListAsTriangleStrip) is an unimplemented TODO in its SPIR-V translator, so the geometry shader is the real path. MEASURED headless on scene frame 600, 3 runs: 19 of 19 rectangle_list draws expanded, 2 distinct geometry shaders (1 and 0 interpolators), 171-174 of 171-174 draws issued, 0 skipped, 917090-921600 of 921600 px non-black; Vulkan validation clean for the geometry stage. It removed BOTH the diagonal split AND the pink tint -- they were one defect: the rectangles are the full-screen colour-grade passes, so half of every one was going ungraded. Zero pixels match the pink test anywhere in the frame. Point lists now use the corresponding Xenia geometry path too: chapter45_recovered has 48, creates a 564-word point GS, and raises zero VUIDs instead of topology-08773 (C071). Those 48 have colour writes masked, so this correction deliberately leaves that frame's final hash unchanged rather than being claimed as its remaining visual fix. scratch/screenshots/rect/frame.png.
- where: runtime/gpu_draw_xlate.cpp (rectangle-list key and shader), runtime/gpu_draw_point_geometry.cpp (point-list key and shader), runtime/gpu_draw.cpp (geometry-stage selection, quad-list index expansion), runtime/gpu_draw_pipelines.cpp (shared geometry module cache and pipeline stages), runtime/gpu_draw_xlate.h
- gap:
- notes:

### draw-backend-colour — Colour path: target format, gamma and swizzle
- status: re-verified
- deps: draw-backend-frame
- evidence: Guest LUT capture and CPU application are retained; GpuScanout now applies the same BuildScanoutGammaLut table on the shared-device GPU path before presentation/readback. Live format-43 capture logged the gamma pipeline, preserved red dominance at mean RGB 49.091/9.220/8.818, and test_scanout_gamma plus test_swapchain_format pass.
- where: runtime/scanout_gamma.cpp; runtime/gpu_scanout.cpp; runtime/gpu_scanout_gamma.cpp; runtime/gpu_present_stage.cpp; runtime/swapchain_format.h
- gap: The measured gamma and swapchain component-layout defects are closed. Remaining colour-variety/render differences belong to frame-first-divergence, not unimplemented scan-out colour handling.
- notes:

### draw-backend-live — Render every frame live instead of one captured frame
- status: re-partial
- deps: draw-backend-frame, draw-backend-primitives
- evidence: The renderer is persistent: RendererPersistent holds the device objects, render target, render passes, descriptor set layouts, shader translations, pipelines, textures, samplers, the command pool/buffer, the fence, the descriptor pool (reset per frame rather than rebuilt), the readback buffer and a persistently-mapped guest-memory mirror. Per-draw uniform blocks and expanded index buffers are suballocated from one persistently-mapped arena sized to the previous frame's high-water mark, with a standalone-buffer fallback on overflow. The pixel readback buffer is HOST_CACHED. MEASURED, scene frame 600 onward, GEARS_DRAW_FRAME_COUNT=10: frame 1 costs 139-248 ms, frames 2-10 cost 15-23 ms, typically 16 -- setup 6, draw loop 4-6, submit+wait 5, readback 0. That is ~62 fps of renderer. 173 of 173 draws issued, 0 skipped, 919286 of 921600 px non-black on the 10th frame; Vulkan validation clean apart from the pre-existing point-list PointSize warning -- re-established 2026-08-05 ON ALL 8 CAPTURES, after this claim went stale and hid seven VUIDs: a depth image cleared without TRANSFER_DST, two invalid SPIR-V constructs in the resolve shaders (`catalog.py show 75`), and every alpha-tested pixel shader declaring a device feature we never enabled (`catalog.py show 76`). Nothing in the routine gates runs the validator, so this line is only as current as the last run of it -- and it must be run over EVERY capture, since the one that was checked first is the one with no alpha-tested shader in it. scratch/screenshots/rect/fast10.png.
- where: runtime/gpu_draw.cpp (RendererPersistent, ReleasePersistent, FrameRenderer, the arena, makeUbo/makeIndexBuffer), runtime/gpu_draw_options.* (one typed configuration snapshot per render), runtime/gpu_draw_indices.* (exact frame-local conversion reuse), runtime/gpu_draw.h (independent FrameDrawInputs::report/probe purposes), runtime/frame_probe_capture.h (diagnostic re-arm and selector-bypass policy), runtime/vd_null_gpu.cpp (GEARS_DRAW_FRAME_COUNT)
- gap: Live rendering every guest frame is correct, and the frame is now FULLY ATTRIBUTED with no dominant item left. MEASURED on a scripted Act 1 gameplay walk (743 draws a frame, mean of the last 800 steady frames): ~44 ms = draw loop ~34 + submit/wait ~6 + readback ~1, and the draw loop is texture upload 8, modification derivation 4-5, shader/layout cache lookups 4, uniforms 4, state's own 4, descriptor-write struct-building 3, prepare 2, index prep 1, ~2 unattributed (shader translation and pipeline CREATION are ~0 once warm -- those are first-frame costs; the driver's vkUpdateDescriptorSets and vkAllocateDescriptorSets are ~0 too, so the descriptor cost is ours, not the driver's). THE PROFILE IS FLAT: nothing left is worth more than 8 ms, so there is no single optimisation that makes this cheap, and chasing the top item would buy at most 8 of 44 ms. THE COST IS ON THE GUEST'S CRITICAL PATH: with live rendering on, gameplay runs at 14-15 fps against 30 with it off (menu frames at ~12 ms still hold 30), so the render halves the title's frame rate. That points at the architecture rather than the items -- taking the render off the swap thread would remove all 44 ms from the critical path, which is worth more than every item on the list combined. Getting there by optimisation instead would mean roughly halving a flat profile. Two instrument defects were fixed getting here: the record region was declared and never accumulated (18 of 36 ms unnamed), and texture upload was reported under state+pipeline though its only call site is inside the descriptor writes. RUN-TO-RUN SPREAD IS ~8 ms -- three runs at an identical 743 draws a frame gave 39.4, 47.2 and 42.7 ms draw loops, and the two SLOWER ones were the runs with 3.6 ms of work REMOVED -- so no change below ~10 ms a frame can be judged by comparing runs. runtime/frame_ab.* interleaves the arms frame by frame inside one run and refuses to report a difference smaller than the noise it could resolve. The same gate now covers every map/set-backed report census, not only viewport strings: on the 1,742-draw chapter-45 frame, census-on is a resolved +1.23 ms (31.77 versus 30.54 ms over 94/94 frames, 1.05 ms floor), so silent frames no longer build those discarded facts.
- notes: THE ARENA NEVER GREW, and that was 60 percent of every frame. The per-draw arena is sized from a high-water mark, and the mark was taken from arenaCursor -- which only advances for allocations that FIT. So it could never exceed the current arena size, the growth test (arenaHighWater > arenaBytes) could never be true, and the arena stayed at its first size forever while 2618 blocks a frame took the fallback that creates, maps, copies and unmaps a VkBuffer each. The log printed "next frame will fit" every frame for thousands of frames and it never did. Fixed by counting the bytes that did NOT fit and sizing from cursor+wanted; the message now says what it will grow to instead of promising a fit. MEASURED: overflows went from 2618 every frame to 3 in a whole run (the arena grows 4882 -> 7432 -> 11900 KiB and then stops), uniforms from 104-111 ms to 4-5 ms, frame cost from 166-179 ms to 55-94 ms. Output unchanged: a presented-frame capture reads 97.6 percent non-black and its channel means are within 1.6 of the saved baseline, against 32.9 if red and blue were transposed. Worth recording how this was found: the uniform packing looked like the culprit at 118 ms, and two measurements said otherwise -- the uniform cache hit rate was 27.7 percent with EVERY miss on the register snapshot pointer, and only 7.9 percent of recomputes produced byte-identical blocks, so caching harder would have bought almost nothing. The cost was never in the packing. Configuration was another distributed fixed cost: Lucent cached values but every lookup still locked and rebuilt the prefixed key. Snapshotting `DRAW_NODEPTHBIAS`, `DRAW_NO_TEX_SIGNS`, and parsed `DRAW_TEX_BINDS` once per render reduced a byte-identical chapter-45 replay's draw loop by 5.13 ms (39.39 -> 34.25 over 44/43 interleaved frames, 2.30 ms resolution). Exact frame-local index reuse then removed repeated conversion/upload before predicated-tile collapse: chapter 45 hit 1,276 of 1,614 reusable draws and reduced 25.09 -> 21.54 ms over 44/43 interleaved frames, a 3.54 ms reduction against 0.81 ms resolution. These are captured-frame renderer results; their live guest-contention effect remains unmeasured (catalog #127, #128). Report-only untile, resolve-plan, and timing diagnostics now remain complete on the evidence frame without being rebuilt or emitted on the preceding frames: the same 101-frame replay reduced log volume 339,050 -> 83,593 bytes, and a focused regression proves diagnostics do not alter collapse output (catalog #138). Texture staleness now skips page-clean guest storage via self-proving soft-dirty tracking (all four alias windows unioned, forced re-verification every 64th frame, auto-disable on sustained contradiction): all 244 chapter-45 textures skipped with byte-identical frames and 0 contradictions in 244 verifications; interleaved A/B resolved -1.27 ms beyond a 0.66 ms floor (catalog #137).

### gameplay-scene — In-game 3D scene rendering (deferred pipeline)
- status: re-partial
- deps: draw-backend-live, draw-backend-rt
- evidence: THE DEFERRED FRAME RENDERS, AND IT IS MEASURED PASS BY PASS AGAINST THE CONSOLE. The backend routes by EDRAM base, mirrors only fetched guest-memory spans, assembles predicated-tile resolves as regions of one texture, and carries colour, depth, stencil, format reinterpretation and colour/depth aliasing. 1X/4X use the expanded EDRAM-sample view; 2X now uses native Vulkan colour/depth attachments with standard diagonal sample locations. Colour k01 resolves average those samples before the existing Xenos exponent-bias/channel-swap conversion; depth resolves use a multisample shader and invert guest 0/1 to Vulkan standard 1/0. On `walk_gameplay.gfr`, the wall-light draw 650 changed from zero fragments to exactly Xenia's 79,253. All 16 resolve passes pair, none is one-sided or coarsely different; the initial 2X scene-colour MAE fell 0.000990 -> 0.000072 and the later scene f32 pass 0.001505 -> 0.000000823 (catalog #115). A full replay and real surface/depth probes are Vulkan-validation clean.
- where: runtime/gpu_draw.cpp (frame orchestration and sample-count routing); runtime/gpu_draw_sample_layout.h (host attachment geometry); runtime/gpu_draw_targets.cpp (sample-count-keyed targets/passes and resolve pipelines); runtime/gpu_draw_resolve.cpp (native colour average and depth sample fetch); tools/layer_compare.py and tools/resolve_exact.py (oracle comparison)
- gap: A future frame that loads existing EDRAM contents immediately after changing one base between native 2X and expanded 1X/4X needs an explicit representation transfer; the verified frame starts each view fresh (catalog #117). The guest's float24 quantisation is not applied to resolved depth (one 20-bit ULP), and non-zero colour clears remain unimplemented because every captured clear is zero. Large world-draw clip rejection is expected under predicated tiling (#30), and the dark frame is the title's art direction, not a colour-path diagnosis.
- notes: `GEARS_DRAW_NOMSAA=1` remains the expanded-grid control arm. The native path is refused on devices without 2X colour/depth/stencil framebuffer support or guaranteed standard sample locations rather than silently reverting to the lossy viewport model. Depth targets now include TRANSFER_SRC because the shipping depth probe copies them directly; the previous plausible-looking dump was Vulkan-invalid (catalog #116).

### renderer-capacity — Persistent EDRAM capacity and shared-present lifetime
- status: re-verified
- deps: draw-backend-live, present
- evidence: Issue #110: two current windowed builds crashed deterministically at frame 6 immediately after the sample grid shrank 1280x1440 -> 1280x720 and ReleasePersistent destroyed a stage image still eligible for presenter blit. After capacity reuse plus shared-device idle on growth, Wayland survived frame 667, X11 survived frame 10109, and live virtual-pad START/A edges drove Press Start -> Main Menu -> Campaign -> Single Player. test_gpu_extent_capacity covers shrink/grow/equal/one-axis growth.
- where: runtime/gpu_extent_capacity.h; runtime/gpu_renderer_capacity.cpp; tests/test_gpu_extent_capacity.cpp
- gap:
- notes: Persistent width/height are allocation capacity, not the current frame's active sample-grid extent. New surfaces allocate at retained capacity.

### gpu-retirement — Publish guest GPU fences only after native rendering retires
- status: re-verified
- deps: cmd-processor, draw-backend-live
- evidence: Catalog #118 and #120 established that EVENT_WRITE_SHD must follow renderer completion and that blocking the command processor collapses the heavy path to 5 fps. Catalog #139 integrates two Vulkan frame slots plus an autonomous ordered completion pump: guest retirement now advances from the producer fence rather than CPU submission. A 25-second real headless-present run crossed the 570-frame load transition, sustained 29.7-30.0 completed frames/s with zero queue drops after warm-up, and emitted no Vulkan validation, shared-image, or publication errors. test_render_retirement still proves that an older completion cannot publish a newer generation.
- where: runtime/gpu_packet_memory.cpp; runtime/render_thread.cpp; runtime/render_retirement.h; runtime/gpu_frame_slots.*; runtime/gpu_frame_timing.*; tests/test_render_retirement.cpp; tests/test_gpu_frame_timing.cpp
- gap:
- notes: This is generation-specific guest-fence publication, not global renderer idleness and not a timed stall. Diagnostics remain synchronous by contract. The recompiled PM4 route is compatibility and oracle infrastructure for the shared GearsUE3 native RHI frontend.

## kernel


### input — Controller input: XamInput* from a host pad, keyboard or script
- status: re-verified
- deps:
- evidence: The XamInput imports fill the console's own structures: X_INPUT_STATE (big-endian packet number + 12-byte X_INPUT_GAMEPAD) and X_INPUT_CAPABILITIES (type/sub_type/flags + a gamepad MASK + vibration ranges), with Xenia's X_INPUT_GAMEPAD_* button bits, hid/input.h. The packet number increments only when the state changes, which is the console's contract. Three host sources: an SDL gamepad, the keyboard, and GEARS_INPUT_SCRIPT (timed button states, so a headless run is reproducible). The pad reports CONNECTED only when a source exists -- a connected pad that never changes reads as a player who is not pressing anything and strands the title at 'press start'. VERIFIED headless: GEARS_INPUT_SCRIPT='25000:START,25300:,26000:START,26300:,30000:A,30300:' fired at 25001/25302/26019/26323/30002/30303 ms; the script only advances when the guest polls, so its firing IS proof the title polls XamInputGetState. The title left the title screen, its draw count went from ~169 to 178-183 per frame, and scratch/screenshots/rect/after_start.png is the MAIN MENU with the 'NO STORAGE DEVICE' dialog, 'PROFILE 1: PLAYER' and the story-mode description -- all consistent with what the other xam stubs report (one local profile named Player, no storage device).
- where: runtime/input.h, runtime/input.cpp (sources + the published snapshot), runtime/xam_user.cpp (XamInputGetState/GetCapabilities/SetState), runtime/gpu_present.cpp (the presenter thread polls SDL), runtime/vd_null_gpu.cpp (InitialiseInput once the window state is known)
- gap:
- notes:

### rtl-time — RtlTimeToTimeFields / RtlTimeFieldsToTime
- status: re-verified
- deps:
- evidence: Both convert against a plain proleptic Gregorian calendar with no timezone or clock scaling, as the RTL routines do: the value is 100 ns ticks since the 1601-01-01 Windows epoch, std::chrono::system_clock uses 1970, and they differ by the fixed 116444736000000000-tick constant. TimeToTimeFields floors to whole days (correct for pre-1970 ticks too), splits with std::chrono::year_month_day/hh_mm_ss/weekday, and writes the eight big-endian X_TIME_FIELDS shorts (year,month,day,hour,minute,second,ms,weekday; weekday 0=Sunday via weekday::c_encoding). FieldsToTime is the inverse and validates the range, returning FALSE like the real routine. UNIT-VERIFIED against known values: 2024-01-01 -> Monday(1), the 1601 epoch -> Monday(1), and 1234567.8901234 s past 1970 -> 1970-01-15 06:56:07.890, matching Python datetime to the millisecond. It was the unblock that let the campaign load: before it, entering a new campaign trapped on the unimplemented import and aborted (SIGABRT); after it the title reaches ACTUAL GAMEPLAY -- see the campaign filmstrip in scratch/screenshots/camp2 (Campaign -> Single Player -> Select Difficulty with character art -> GEARS OF WAR loading screen -> 'ASHES 14 YEARS AFTER E-DAY' -> in-game HUD 'Exit the cell area').
- where: runtime/kernel_rtl.cpp (__imp__RtlTimeToTimeFields, __imp__RtlTimeFieldsToTime), runtime/implemented_imports.h
- gap:
- notes:

## audio


### audio-driver — Render-driver frame path: the title produces PCM
- status: re-verified
- deps:
- evidence: 60 s run with GEARS_AUDIO_PUMP=1: 11250 callback invocations and 11250 frames submitted, one for one (exactly 60 s at the driver's 187.5 Hz); 2763 frames non-silent, peak 0.518, ffmpeg volumedetect mean -27.4 dB / max -5.7 dB on the GEARS_AUDIO_WAV dump
- where: runtime/xaudio_null.cpp (pump, submit, peak measurement, WAV dump); runtime/kernel_object_api.cpp (KeWaitForMultipleObjects); runtime/guest_thread.cpp + kernel_thread.cpp (processor number from the title)
- gap:
- notes: The blocker was the guest thread's PROCESSOR NUMBER, not the audio API: the title's audio worker checks into a rendezvous barrier at array[KPCR+0x10C] and we invented that number instead of taking it from ExCreateThread's creation flags. Catalog #40. WHAT THIS EVIDENCE DID NOT COVER (2026-08-04): counting frames and measuring their peak says the title PRODUCED audio, not that we read it correctly. The frame is six PLANES of 256 big-endian floats and we read it as interleaved for months -- audible as wildly wrong pitch and a click every 256/6 samples, invisible to every number in the evidence line above, and invisible to the WAV dump too because the dump wrote the same planes into an interleaved container. Fixed and measured in catalog #55; the test that pins the layout is tests/test_audio_frame.cpp.

### xma-decode — XMA hardware decode
- status: re-verified
- deps: audio-driver
- evidence: tools/xma_replay drives the dumped streams through the production xma_context.cpp and the output matches an independent ffmpeg decode: ctx1 correlation 1.000000 over the full 141.84 s (rms diff 0.000023, peak 0.000061, constant 576-sample priming lag), ctx0 correlation 1.000000 over 8.34 s. Reproduced by the operator from a clean build. The comparator was validated against a case that must differ (reversed audio: correlation 0.081, exit 1). Live: a run to gameplay is 81.8% non-silent versus 25% before, with zero xma warnings, and decode costs 1.59 s across 71000 kicks (22 us mean).
- where: runtime/xaudio_null.cpp: XMACreateContext hands out a zeroed 0x40-byte context in physical memory and nothing decodes
- gap: Loop playback, true double-buffer streaming and non-44100/24000 rates are ported from the reference but exercised by no stream yet. Mono decodes live (contexts 23-25) but has not been compared against a golden. No output device: nobody has heard this on speakers.
- notes: Marked hack rather than missing because the context handout LOOKS like support and the title behaves as if decode were coming. Catalog #39.

### saves-content — Saved-game content: profile, content, directory handles
- status: re-verified
- deps: rtl-time
- evidence: The title now walks its own save path end to end and WRITES A SAVE FILE: `created 'save:\Pla' for writing`. Each layer is the console's, tested against its own contract (8 unit suites): profile settings over a real persisted store (setting ids self-describe as id:16/size:12/type:4; an unset setting reports NO_VALUE/UNSET so the title can tell "never set" from "set to zero"); XCONTENT_DATA at its real 0x134 layout; the console's creation dispositions, where OPEN_EXISTING is 3 and the title sends 0x13 — having it as 2 made every open-existing request answer as a creation; XamContentCreateEx completing its XOVERLAPPED and returning ERROR_IO_PENDING, because the call site branches on 997 and skips the entire load otherwise. Measured consequence: the title now issues OPEN_EXISTING then, on NOT_FOUND, CREATE_NEW — a second call it never used to make.
- where: runtime/user_profile.cpp, runtime/xam_content.cpp, runtime/xam_user.cpp, runtime/directory_info.cpp, runtime/kernel_file.cpp
- gap: WRITING is now verified on real data: the file is 385 bytes, BYTE-IDENTICAL to the disc checkpoint `WarGame/Checkpoints/chapter37.sav` it originates from, and decodes as a well-formed UE3 record — version 2, chapter 0x25 (37, matching the source filename), then a length-prefixed FString of 12 bytes reading "sp_prison_p" (the Act 1 map). So the round trip disc -> Kismet carrier -> player save is correct end to end. What remains untested is LOADING one back: the checkpoint restore path crashes first (catalog #45), so no save has ever been read into the title. XamUserGetName truncates as the console does, which is why the file is named "Pla" from a 4-byte buffer.
- notes: XConfig was shifted by two from 0x07 up, so the title asking for LANGUAGE got the audio-flags entry and read 0 where English is 1. Fixed and pinned by name-to-number tests, because a bare-number table fails invisibly — every lookup still succeeds and answers a different question.

### thread-races — Game/render thread races the console tolerated
- status: re-verified
- deps: saves-content
- evidence: RESOLVED, and it was never a race. Root cause: XamContentCreateEx read the overlapped block out of r9, which is cacheSize -- the real signature has NINE parameters and the overlapped is the ninth, on the stack at r1+84. The title own wrapper at 0x82611900 spills it there and zeroes r9, so we saw no overlapped, completed synchronously and returned success. The checkpoint loader at 0x821B6800 requires ERROR_IO_PENDING (it compares against 997 and exits otherwise), so it reported success having loaded nothing, its caller skipped copying the 385-byte carrier, and the empty archive produced NAME_None -> a request for a package called None -> the retail use-after-free in ULinkerLoad::CreateLoader. Verified on the repro: FName now resolves to sp_prison_p index 0x775f, the real level packages load (SP_Prison_World, SP_Prison_S01), 8640 frames with zero faults and zero bad indirect calls, where before it aborted at ~1560.
- where: nothing in runtime/ — the racing code is all the title's
- gap:
- notes: Fixed in runtime/guest_stack_argument.{h,cpp} plus the corrected parameter mapping in xam_user.cpp, tests first. FOUR wrong theories were recorded against this step before the answer and are listed in catalog #45 so they are not re-derived: a game/render thread race, GPU pipeline latency, the holder being freed, and safe-by-allocator-timing. Also cleared as suspects: the gamertag-derived save filename (the read and write share the path builder, so save:\\Pla was always self-consistent) and the default_checkpoint.sav literal (a fallback not meant to be reached). #44 is NOT downstream of this -- the ring two-producer warnings still occur 421 times in the fixed run -- so it stands as its own bug.

## compare


### frame-pair-validated — A cross-console pair that PASSES the same-picture gate
- status: re-verified
- deps:
- evidence: tools/camera_pair.sh produces one oracle run plus our camera-gated capture from THAT run, both provenance-stamped with one pair id and the camera frozen in. scratch/camerapair_rot scores 0.6082 colour against console frame 790 (control 0.9371, gate 0.60), gate matched at 0.13 thresholds with rotation 0.0013 and translation 0.00057 of magnitude. Depth on the earlier pair 0.8900. Yardstick: the console against ITSELF one frame apart scores 0.297 colour / 0.621 depth.
- where:
- gap:
- notes: Every pixelwise cross-console number depends on this step. Before it, three separate pairings were measured and all failed: joining across runs 0.07 (C042), the content selector 0.49 (C043), the orientation-blind camera metric 0.376 (C045).

### frame-first-divergence — Walk the frame in execution order and name the first pass that loses agreement
- status: re-partial
- deps: frame-pair-validated
- evidence: The former gross C400 divergence and draw-743 localization are withdrawn: Xenia's trace dump was using asynchronous first-use placeholder shaders. Current code replayed `chapter45_recovered.gfr` against the synchronous oracle and paired all 24 compatibility-renderer handoffs with zero one-sided passes. All colour rows now have 0.00% of available pixels over the 0.1 threshold, including the former 0.41% downstream composite. `layer_compare_ranges` proves a unique legacy texture base from the complete D5A0 sibling and makes all 11 shadow-depth copies comparable; all 12 depth handoffs match, with copy #0 exact where both sides wrote. The largest former defect was the C5A0 logical-width error fixed in catalog #114.
- where: runtime/gpu_resolve_extent.*; runtime/gpu_draw_resolve_plan.cpp; runtime/gpu_draw_targets.cpp; runtime/gpu_draw_textures.cpp; tools/layer_compare.py; tools/layer_compare_ranges.py; tools/resolve_exact.py; tools/gfr_to_xtr.py; tools/gfr_trace_plan.py; extern/xenia/src/xenia/gpu/vulkan/vulkan_trace_dump_main.cc; scratch/frames/chapter45_recovered.gfr
- gap: Exact half-float bits still differ at C400 despite no threshold-visible pass residual. Localize that numerical delta only with the exact synchronous corpus and only if it propagates into a consumer; broaden the same 24/24 gate to a materially different paired scene. Do not reopen C400 from an async trace dump or rely on I051/I054 zero checkpoint content.
- notes: Guest destination pitch and logical sampled width are independent quantities. The raw half-float comparator remains the strict numerical falsifier; the pass comparer is the visual-severity gate, and neither may be substituted for the other.

### hdr-pair-ui-state — Acquire a UI-state-matched HDR oracle/native pair
- status: re-verified
- deps: frame-pair-validated
- evidence: scratch/camerapair_short_ui_20260813 from the shipping camera_pair.sh path with the f450 START/f600 A route after implementing Xenia's XN_SYS_UI open/close protocol. Provenance MATCH with frozen camera digest 5836bee14961d066; both input arms fired; oracle camera f901; native camera matched at 0.61 camera thresholds. Decoded front buffers visibly show the same door and no overlay. A pre-fix frame visibly carries NO STORAGE DEVICE; the post-fix capture shows it gone and CHECKPOINT rendered.
- where: tools/camera_pair.sh; tools/resolve_float_stats.py
- gap:
- notes: The acceptance gate is provenance + camera-state match + scripted-input validation + explicit overlay/state inspection. Renderer work starts at the earliest exact draw/state divergence; no aggregate image score certifies the pair.

## render


### f7-first-loss-localisation — Localise the first f7 resolve's inherited or produced difference
- status: re-verified
- deps: hdr-pair-ui-state, frame-first-divergence
- evidence: The former correlation-scored conclusion was withdrawn with C053. On the current synchronous chapter-45 rebaseline the f7 handoff matches with 0.00% of available pixels over 0.1, and every earlier structural handoff does too. There is no threshold-visible f7 loss to localize in this capture.
- where: tools/layer_compare.py; tools/draw_interval_ledger.py
- gap:
- notes: Score-based pair_score.py and first_divergence.py were removed to prevent aggregate metrics from being mistaken for a first divergence.

### diverse-scene-pair-coverage — Verify rendering across materially different gameplay views
- status: re-partial
- deps: f7-first-loss-localisation, mask1-missing-shading-draw
- evidence: Chapter 45 adds the outdoor checkpoint. Full authored-mip upload restores identity-matched shadow casters to 31,621 versus oracle 31,618 pixels and 12,147 versus 12,147, with sampled value 0.968719 on both renderers (C063, catalog #109). The fresh post-fix pair has 21/21 structurally shared resolves. The TITLE SCREEN is now verified at matched animation state: the procedural fire pass (vs 15dbe06e53bd0f02/ps 32091b4c63cda933) compares 0.00% of pixels differing against an oracle run whose camera constants equal ours, and the depth pass matches exactly (catalog #88). A PS-override probe series proved the translated shader equals a hand transcription of the microcode before the state-matched pair was taken. A NEW chapter-45 pair (2026-08-25, both guests through the title's own campaign path with the checkpoint-45 save, camera-gated on cb3cec323318973e/c8, provenance-matched) pairs 24 resolves: the final composite matches at 0.00% of pixels differing, the light pass 0.90%, the three bloom tiles under 1.25%, depth agrees where both wrote (catalog #108).
- where: tools/camera_pair.sh; tools/layer_compare.py; tools/draw_interval_ledger.py
- gap: In the 2026-08-25 chapter-45 pair, two shadow-atlas resolves (srcD5A0 864x864 f22 #12 and #13) exist ONLY on our side in every one of the console's five dumped frames - the named first renderer lead. The UI-state gate refuses the pair because the tutorial toast appears somewhere in the console's five-frame window but not at our captured frame (one-frame-vs-window asymmetry for a transient overlay); a full pixel pair needs our capture inside the tutorial window or a frame-vs-frame form of that check. In the earlier cached chapter-45 pair, the first 40 draws align exactly after normalizing disabled fragment stages; the four caster identities are the same but their title-issued order differs, so that is world-state drift rather than renderer behavior. Keep the known different 8,448-index caster, particle/effect-heavy passes, and structurally undecodable rows outside parity denominators. Title-screen comparisons must pair on animation state (pass output or camera constants c8..c11), never frame index: the fire scale and camera animate on guest time and the oracle's own phase differs between runs. Xenia is verified only for Gears 1; Gears 2 and 3 need independent oracle validation.
- notes: A 301-frame rendered warm run landed on the same nominal native guest frame 5901 and left the first D5A0 mean exactly 0.9512, unchanged from the one-frame capture and against oracle 0.9322. Therefore resetting host depth targets at frame boundaries is not this residual's cause; the proposed persistence change was removed. Continuous movement is essential for path crossing: bounding LY+ by frame leaves permanent unequal endpoints because UE3 integrates wall-clock delta time (#106/#84). Do not loosen the camera gate to compensate.

### mask1-missing-shading-draw — Shadow masks match after depth-scale and split-depth fixes
- status: re-verified
- deps: frame-first-divergence
- evidence: Resolved by the depth-scale and split-depth fixes, measured on a camera-gated capture against console frame 793: mask #0 correlation 0.9899 with 11.35% shadowed against 11.39%; mask #1 correlation 0.9987 with 4.85% shadowed against 4.88%, and 33 distinct values on both sides. The later four-arm A/B makes the memory-model cause unambiguous: shared and shared-control leave mask #1 flat 1.000, while split scores 0.9937 and the shipped default 0.9971.
- where: runtime/gpu_draw.cpp; runtime/gpu_draw_resolve_decode.cpp
- gap:
- notes: The earlier 0.2681 camerapair_rot frontier and per-draw light-count theory are historical, predating the half-scale depth fix. Do not reopen them from that capture. Issue #91 records the full causal chain and the later A/B.

## gearsue3-engine


### clean-distribution-tip — Remove private-source and game-derived build inputs from the tracked tip
- status: re-partial
- deps:
- evidence: The external-source Core build island and its generator/tests have been removed. After the approved rewrite, a fresh single-branch public clone at `bf2e829f0043237909b00b2b1722369b1cb5fb9a` passed both tracked-tip and `--history` modes of `tools/check_distribution_clean.py`, and `origin/main` resolved to that commit. The gate rejects private-source inputs, generated recomp bodies, title caches, and game/media artifacts; its self-test includes both answers.
- where: AGENTS.md; docs/gearsue3-engine.md; tools/check_distribution_clean.py; .gitignore
- gap: Native shader/protocol provenance and third-party notices still need a complete audit before the repository can be called clean. Any newly pushed commit or ref requires the history gate again.
- notes: The history rewrite is complete; do not carry its former pending status forward. The full-history release gate remains independent of the tracked-tip CTest.

### recomp-forwarding-seam — Keep every recomp body while making all direct calls overridable
- status: re-verified
- deps: clean-distribution-tip
- evidence: XenonRecomp commit 884206f emits a retained implementation and weak noinline forwarder for every function. Its multi-TU regression proves a generated same-TU call reaches a strong override while the override can super-call the retained body. A fresh Gears 1 generation emitted 48,892 implementations and 48,892 forwarders with zero compiler aliases.
- where: extern/XenonRecomp/XenonRecomp; runtime/hle_d3d.cpp
- gap: Runtime A/B selection remains per override rather than one central engine registry; build-specific bindings still need the title-revision boundary below.
- notes: Generated output remains ignored and unmodified. The original `__imp__*` body is never removed.

### recomp-disjoint-switch-cfg — Preserve computed-dispatch ownership across inline data
- status: re-verified
- deps: recomp-forwarding-seam
- evidence: Function ownership is now a normalized set of executable blocks. Configured switch labels seed bounded CFG discovery only inside executable, non-data ranges and cannot cross an authoritative foreign function envelope. A fresh exact Gears generation reached 100% with zero `ERROR`, unreachable-switch, or unrecognized-instruction matches. Each of the eight targets beyond the two inline address tables emitted as one local label with one dispatch edge, no target emitted as a standalone function, and no table-word address emitted as a function. Focused red/green discovery and emission tests pass in the aggregate 9/9 sanitizer CTest suite.
- where: extern/XenonRecomp/XenonAnalyse/function.*; extern/XenonRecomp/XenonRecomp/data_range.*; extern/XenonRecomp/XenonRecomp/function_scan.*; extern/XenonRecomp/XenonRecomp/switch_extent.cpp; extern/XenonRecomp/XenonRecompTests; config/gears.toml
- gap: This verifies the exact configured Gears revision. Other titles and revisions still need their computed dispatches detected and their factual inline-data ranges supplied; an unconfigured pattern must continue to fail generation rather than being absorbed by extent.
- notes: Case blocks are not callable functions. The retired maximum-target repair crossed data and could absorb unrelated code; do not restore it or add case labels to the manual function list.

### title-revision-boundary — Separate shared engine behavior from exact title bindings
- status: in-progress
- deps: recomp-disjoint-switch-cfg
- evidence: `runtime/title_profile.*` owns the shared exact-identity schema and fail-closed resolver. XenonRecomp emits effective-container SHA-256, checked normalized-image SHA-256, base, size, and entry point into the ignored PPC module; `runtime/title_executable.*` loads and hashes the selected XEX once, and `runtime/generated_title_profile.*` compiles that identity into the linked profile. Startup resolves it before save state, guest memory, function mappings, imports, or guest entry. Synthetic tests refuse same-layout digest changes; a fresh retail generation emitted the independently verified digests and compiled all 191 translation units after overlapping CFG blocks were normalized for emission. The Gears 1 adapter at runtime/titles/gears1/rhi_bindings.cpp now owns seven exact guest entry points and all device offsets for normal draws, texture binding, and shader binding; the title-neutral semantic stream owns none of them.
- where: runtime/title_profile.*; runtime/title_executable.*; runtime/generated_title_profile.*; runtime/main.cpp; runtime/guest_filesystem.*; tests/test_title_profile.cpp; tests/test_generated_title_profile.cpp; tests/test_title_executable.cpp; extern/XenonRecomp/XenonRecomp; extern/XenonRecomp/XenonUtils/sha256.*
- gap: Other Gears 1 guest-address bindings, shader/pass hashes, probes, and scripted policy remain compiled into shared-looking runtime files rather than factual title adapters. Exact runtime activation is closed, but the ownership separation is not complete.
- notes: One executable links one locally generated title module. Exact parsed-image identity must fail closed before any build-specific binding activates.

### rom-only-provisioning — Generate a playable title from one user-owned disc/image
- status: in-progress
- deps: title-revision-boundary
- evidence: `tools/title_identity.py` and its 16 synthetic tests implement strict input priority, streaming disc SHA-256, duplicate-key/schema refusal, one checked `xex-inspect` authority, independent container/normalized-image re-hashing, and path-free JSON under ignored `scratch/titles/<disc-sha256>/`. The hardened GDF extractor's 13 tests cover bounded parsing, traversal/cycle/collision refusal, symlink confinement, exact reads, verified resume, and atomic publication. The retail XEX passes ASan/UBSan inspection with 17 sections and 236 logical imports; a fresh recompile now seals its exact container/image identity into the locally linked runtime profile.
- where: tools/title_identity.py; tests/test_title_identity.py; tools/gdf_extract.py; tests/test_gdf_extract.py; extern/XenonRecomp/XexInspect; config; run.sh
- gap: Extraction, switch analysis/merge, recompilation, validation, build, and launch are still separate manual steps. There is not yet one deterministic command or path-free receipt from a clean clone to a playable executable; CMake and `run.sh` still select game and PPC paths independently even though startup now refuses any cross-revision executable.
- notes: The identity record is the first boundary, not a provisioner completion claim. Generate every disc-derived artifact under the content-addressed ignored directory and write a tool/input receipt. Unknown revisions refuse.

### frame-delivery-contract — Carry one guest frame identity through a bounded latest-frame pipeline
- status: re-verified
- deps: gpu-retirement
- evidence: Production tests cover the latest-pending CPU queue, monotonic frame contract, bounded GPU retirement policy, queue serialization, retained shared-frame ownership, and modular timestamp-counter wrap. Two Vulkan slots own every frame-mutable renderer resource, timestamp query pair, and deferred cleanup through ordered producer-fence completion. The timestamp instrument proved both answers on identical input: chapter 45 measured 13.537 ms for 1,742 draws versus 1.917 ms for the one-draw control, with zero query failures. A Release live headless run sustained 29.8-29.9 completed frames/s with zero queue drops and measured ordinary title frames at 7-8 ms GPU. Removing the unused headless host readback reduced steady renderer CPU from 10-12 ms to 5-6 ms per frame. The synchronous chapter-45 replay remains pixel-identical at SHA-256 `3b34082ab05198fa4733a50c7fe6e671b32c3871be38f7b563154ec741f80c25`.
- where: runtime/frame_queue.*; runtime/frame_contract.*; runtime/gpu_queue_access.*; runtime/gpu_retirement.*; runtime/gpu_frame_slots.*; runtime/gpu_frame_timing.*; runtime/gpu_frame_cleanup.*; runtime/render_thread.*; runtime/gpu_scanout.*; runtime/gpu_shared_device.*; runtime/gpu_present_source.*; tests/test_frame_queue.cpp; tests/test_frame_contract.cpp; tests/test_gpu_frame_timing.cpp; tests/test_gpu_queue_access.cpp; tests/test_gpu_retirement.cpp; tests/test_shared_frame_image.cpp
- gap: The live title remains capped near 30 produced frames/s. The measured compatibility renderer clears 16.67 ms on these workloads but not the native engine's 5 ms/200 fps target, and no 60 Hz title simulation enhancement exists.
- notes: CPU queue replacement, GPU slot overlap, guest retirement, and presenter consumption are four distinct lifetime boundaries. Diagnostic report/probe/replay frames wait explicitly and do not use the deferred readback path. The compatibility result is a migration baseline, not the native product's performance ceiling.

### title-60hz-enhancement — Produce title simulation and frames at 60 Hz
- status: todo
- deps: multi-title-conformance, renderer-60hz-budget
- evidence: With rendering effectively disabled, Gears 1 remains near 30 fps. The host vblank source is already 60 Hz, so renderer removal and vblank frequency do not by themselves remove the title-side frame-production limit.
- where: title/revision timing bindings; runtime guest timing services; headless timing A/B reports
- gap: Identify the semantic limiter in each exact executable revision, then implement a runtime faithful/enhanced A/B seam. No verified limiter or 60 Hz title enhancement exists yet.
- notes: This is the last-priority per-game enhancement after Gears 1 is stable and performant at its faithful cadence and the engine compatibility work is established. Do not speed the general guest clock, alter vblank to hide the cap, or patch an unexplained constant. Catalog #126 records two rejected shortcuts: forcing live D3D sync mode 2 to mode 1 remained near 30 presents/s, and the only exact 1/30 fixed-step branch was disabled at runtime. The semantic limiter is still behind the indirect game-thread/render-command producer chain upstream of Present. Gears 1 evidence cannot establish the Gears 2, Gears 3, or Judgment timing policy.

### native-rhi-observation — Mirror semantic D3D/RHI operations while super-calling recomp bodies
- status: in-progress
- deps: title-revision-boundary
- evidence: Scoped alias-aware guest write attribution and raw packet-construction scans grounded four normal Gears 1 draw entry points. runtime/titles/gears1/rhi_bindings.cpp retains and super-calls the draw/texture bodies; the focused shader-setter owner keeps the shader bodies as runtime controls while implementing the exact native state transition. A headless menu walk through frame 1712 matched 90,854/90,854 draws; a binding run matched 970/970 updates. One typed frame event vector now preserves draw/binding interleaving directly, and the focused test proves the draw/binding/draw order plus both comparison answers. `VdSwap` appends the terminal semantic present through the extracted `gpu_swap_packet.*` compatibility transport owner; a live headless run through frame 240 matched 236/236 draws, 1,914/1,914 bindings, and 240/240 presents with no missing or mismatched observation. The transactional shader-setter audit matched 240/240 exact write sets with zero fallbacks and a focused mismatch negative control. Its timing gate rejected default native execution: 1273 ns native versus 1232 ns retained median over 176/176 calls.
- where: runtime/rhi_semantic_stream.*; runtime/gpu_swap_packet.*; runtime/titles/gears1/rhi_bindings.cpp; runtime/titles/gears1/shader_setter_{state,override}.*; runtime/guest_write_watch.*; docs/d3d-seam.md; tests/test_rhi_semantic_stream.cpp; tests/test_gpu_swap_packet.cpp; tests/test_shader_setter_state.cpp
- gap: The bound-vertex entry is statically grounded but lacks live coverage. Complete state, resource lifetime, resolves, retirement, full-stream comparison, and pixel parity do not yet exist, so no native bypass is authorized.
- notes: Logical title draw calls are not one-to-one with compatibility-renderer draw executions because predicated Xenos packets replay per EDRAM tile. Exact guest addresses and device offsets remain in the Gears 1 adapter; the semantic stream is title-neutral.

### native-rhi-bypass — Bypass guest D3D/PM4 work only after faithful parity
- status: todo
- deps: native-rhi-observation
- evidence:
- where: runtime; tests; tools/frame_replay
- gap: No runtime original/native toggle or state/pixel parity gate exists for a complete RHI frontend.
- notes: Preserve the PM4/recomp arm in the same binary. Performance is measured only after the faithful arm passes, with a deliberately wrong control that the comparer rejects. The approximately 5 ms/200 fps renderer target belongs here: native execution must remove Xenos PM4 parsing, EDRAM reconstruction, and redundant pass reconstruction rather than require the compatibility oracle to carry that architecture at native-PC cost.

### renderer-60hz-budget — Render steady gameplay below 16.67 ms per produced frame
- status: todo
- deps: frame-delivery-contract, native-rhi-bypass
- evidence: Before slot integration, a 101-frame chapter-45 replay measured median 53 ms total, 36.5 ms CPU draw loop, and 14 ms submit+wait. Catalog #139 removes that unconditional wait. Catalog #140 adds per-slot full-command-buffer timestamps and proves them against a one-draw control. The captured 1,742-draw chapter-45 frame executes in 13.537 ms; live ordinary title frames execute in 7-8 ms with zero failed samples. A Release headless run after removing its unused full-frame readback uses 5-6 ms renderer CPU and drops no frames.
- where: runtime/gpu_draw*; runtime/frame_queue.*; runtime/gpu_retirement.*; native RHI frontend; headless frame-time reports
- gap: These bounded workloads establish a sub-16.67 ms compatibility-renderer budget, not sustained interactive gameplay or the native engine's 5 ms/200 fps target. Integrate the native RHI path, remove the separate title-side 30 Hz production limit, then demonstrate sustained gameplay with correct retirement and no dropped evidence frames. No title currently passes the end-to-end gate.
- notes: A 60 Hz vblank, a repeated presentation, or an average inflated by menu frames is not a 60 fps gameplay result. Measure produced gameplay frames and preserve the compatibility arm for parity. A 7-8 ms compatibility frame is useful evidence, not an acceptable native-renderer ceiling.

### multi-title-conformance — Verify each Gears UE3 title independently
- status: in-progress
- deps: rom-only-provisioning, native-rhi-bypass
- evidence: `tools/title_conformance.py` and its synthetic self-test distinguish Gears 1, Gears 2, Gears 3, and Judgment; require exact manifest/result identity and digest-bound relative artifacts; report sustained 60 fps gameplay as a separate enhancement rather than a compatibility prerequisite; fail missing or incompatible evidence; and reject Xenia-sourced evidence for every non-Gears-1 title. Only Gears 1 currently has real headless boot, menu, gameplay, compatibility-renderer, and narrowly validated oracle evidence.
- where: tools/title_conformance.py; tests/CMakeLists.txt; docs/gearsue3-engine.md; local title manifests and headless conformance reports
- gap: No real exact-build compatibility report has passed. Gears 2, Gears 3, and Judgment have no local revision identity, recompiler coverage, package corpus, headless content/menu/gameplay evidence, or renderer compatibility/native parity. No title has separate 60 fps enhancement evidence.
- notes: The reporter is implemented; compatibility is not. Shared engine code is a design target, and support remains per exact revision and per gate. Xenia is not an accepted oracle for Gears 2, Gears 3, or Judgment.

# Codemap

Orientation map for gears1. Consult this **before** starting work to find where
a subsystem lives and how far it actually got; update it in the **same commit**
that lands or changes a subsystem.

Status vocabulary — deliberately narrow, so it cannot flatter the project:

| Status | Meaning |
|---|---|
| **real** | Implemented and verified against real data. Does what its name says. |
| **partial** | Genuinely implemented, but with known gaps. Gaps named. |
| **null** | Deliberately does nothing while presenting a working interface. Not a stub to be filled in later by accident — a *documented* absence. |
| **absent** | Not implemented. Traps loudly if called. |

## Where things are

| Path | What it is |
|---|---|
| `config/gears.toml` | XenonRecomp configuration: XEX path, save/restore helper addresses, switch-table file |
| `tools/gdf_extract.py` | GDF/XDVDFS disc reader. `--list`, `--extract`, `--extract-all` (resumable) |
| `tools/xex_probe/` | XEX decrypt/decompress, section + import dump, save/restore helper byte-scan |
| `tools/dedupe_recomp.py` | Removes duplicate/stale generated TUs. `--check` in CI: duplicate `__imp__sub_X` definitions make the link order-dependent |
| `tools/prepare_overrides.py` | Strips the weak alias for functions listed in `runtime/hle_d3d.cpp`, so intra-TU calls reach a native override. **Re-run after changing the override list** |
| `tools/gen_import_stubs.py` | Emits a trapping stub for every import not named in `implemented_imports.h` |
| `extern/XenonRecomp` | Submodule → our fork, branch `gears` |
| `xenia_gpu/` | Build island for Xenia's Xenos microcode front end + SPIR-V back end out of `extern/xenia`. One CMakeLists plus `xenia_host_shim.cpp` — no Xenia sources are copied. Optional at configure time exactly like the Vulkan/SDL3 presenter |
| `tools/shader_extract.py` | Scans any file for `0x102A11tt` shader containers, validates the layout, and writes deduplicated containers. The container layout is documented in its docstring |
| `tools/xenos_translate/` | Offline driver: container (or, with `--raw`, bare captured microcode) → Xenos microcode → SPIR-V + microcode disassembly. Measurement tool, not part of the runtime |
| `tools/system_constants/` | Offline driver: a register-file snapshot (`GEARS_CONST_DUMP` → `scratch/bin/regfile_hotpair.bin`) → the 528-byte `xe_uniform_system_constants` UBO. Ports Xenia's `UpdateSystemConstantValues` (non-FSI) reading our tracked registers; reuses Xenia `draw_util::GetHostViewportInfo` for NDC scale/offset. Verified byte-exact against the bound shader's SPIR-V member offsets. Optional at configure time, links `xenia_gpu` |
| `tools/compare_bound_shaders.py` | Compares runtime-captured microcode against the offline container corpus and ranks the captured set by bind count |
| `runtime/` | The PC-side runtime. See below |
| `tests/` | `test_vmx_instructions` (fork's instruction implementations) and `test_runtime_logic` (kernel object semantics, path translation). Both mutation-checked |
| `tools/decode_bc.py` | Decodes a raw BC1/BC3 blob dumped by `GEARS_DRAW_TEX_DUMP=1` to a PNG. Exists to check the guest-texture **decode** (detiling + endian + block layout) independently of the renderer — if these look like game art, any remaining blackness is downstream |
| `tools/catalog.py` + `docs/issues/` | **Findings registry, keyed by symptom. Search it before investigating anything.** `catalog.py search "<symptom>"` |
| `debug_journal/` | Dated narrative of each session. The catalog is the index into it |
| `scratch/` | All derived output, gitignored: `ppc/` generated C++, `game/` extracted disc, `bin/`, `logs/`, `raw/` |

## Runtime subsystems

| Area | Files | Status | Notes |
|---|---|---|---|
| Guest memory | `guest_memory.*` | **real** | 4 GiB sparse window. Physical RAM is one `memfd` aliased at `0x0/0xA0/0xC0/0xE0000000` because guest code converts between them by masking the top 3 bits |
| Heaps | `guest_heap.*` | **works** | Page-granular, honours requested alignment. Address-keyed free list, first fit, coalescing on free; freed pages stay committed but recycled address space is re-zeroed before it is handed out (issue #18). Verified plateau at 192/194 MiB over 25 min of gameplay |
| Image loading | `main.cpp` | **real** | XEX via XenonUtils; refuses an image whose layout differs from the recompiled code's |
| Indirect calls | `guest_memory.cpp` | **real** | 49,475 functions installed into the `PPC_LOOKUP_FUNC` table |
| Variable imports | `import_variables.*` | **real** | 236 resolved. XenonUtils leaves these unresolved upstream; fixed in our fork |
| Threads | `guest_thread.*`, `kernel_thread.cpp` | **real** | Per-thread `PPCContext`/KPCR/TLS/stack. `CREATE_SUSPENDED` is a real gate. Affinity deliberately **not** honoured |
| Sync | `kernel_sync.cpp`, `kernel_spinlock.cpp` | **real** | Critical sections and spin locks are real host locks, correct under real threading |
| Kernel objects | `kernel_objects.*`, `kernel_events.cpp`, `kernel_object_api.cpp`, `kernel_dispatcher.cpp` | **partial**, tested | Events + semaphores, handle table, and guest-memory dispatcher objects bound lazily from their own header. **`ObDereferenceObject` is a no-op** — objects outlive their refcount |
| File I/O | `guest_filesystem.*`, `kernel_file.cpp` | **partial** | Real reads from extracted disc files, case-insensitive fallback. No directory enumeration, no writes |
| Memory API | `kernel_memory.cpp` | **partial** | Virtual + physical allocation. Page protection recorded, **not enforced** |
| Time | `kernel_time.cpp` | **real** | 50 MHz Xenon timebase |
| Config | `kernel_config.cpp` | **partial** | Answers only settings with a defensible value; refuses the rest **by name** |
| Strings | `kernel_rtl.cpp` | **partial** | Counted strings, code-page conversion, memory fills. `X_ANSI_STRING` parsing verified |
| Display | `kernel_video.cpp` | **partial** | Reports 1280x720p60 widescreen. Verified that the title's layout does **not** depend on this |
| **Input** | `input.cpp`, `xam_user.cpp` | **real, verified on real data** | `XamInputGetState`/`GetCapabilities`/`SetState` fill the console's own `X_INPUT_STATE`/`X_INPUT_CAPABILITIES`, with the packet number incrementing only on a real change. Three host sources: an **SDL gamepad**, the **keyboard** (Enter=START, Space=A, WASD=left stick, arrows=d-pad), and **`GEARS_INPUT_SCRIPT`** (`"25000:START,25300:"` — timed button states, so a headless run is reproducible). The pad reports connected **only when a source exists**: a connected pad that never changes reads as a player not pressing anything and strands the title at PRESS START. Verified: a scripted START took the title into the **main menu** (`scratch/screenshots/rect/after_start.png`), draws/frame 169 → 178–183 |
| **HLE D3D** | `hle_d3d.cpp` | **partial** | The native-override seam for the guest D3D layer, with a per-frame call census carrying call-site provenance (channel `hle`). Overriding **works** now -- it did not before `tools/prepare_overrides.py`, and a strong override linked cleanly while never being entered. ~60 guest functions are wrapped so far, all for instrumentation (census, worker-queue watchpoint, shader argument scan); none replaces guest behaviour |
| **GPU** | `vd_null_gpu.cpp` | **command processor, renders nothing** | Consumes the ring, follows indirect buffers, and executes TYPE0 register writes, `EVENT_WRITE_SHD` fences, `EVENT_WRITE_ZPD` occlusion reports, `WAIT_REG_MEM`, `INTERRUPT` (dispatched per CPU) and **predication** via bin mask/select. No draw is ever performed and no pixel is produced. Vblank at 60 Hz (`GEARS_NO_VBLANK=1` disables). `GEARS_CP_STALL_MS=N` blocks the CP thread for N ms at the first swap doing nothing else — the control arm for "did our host-side work perturb the guest?"; measured, a 20 s stall costs throughput only and induces no guest error path (issue #26) |
| **Guest-draw backend** | `runtime/gpu_draw.cpp` (Vulkan renderer), `runtime/gpu_draw_xlate.cpp` (Xenos→SPIR-V + Modification + SystemConstants, isolated static lib) | **real, verified on real data** | **The renderer is persistent and drives the frame live**: `GEARS_DRAW_FRAME_COUNT=0` renders EVERY frame from `GEARS_DRAW_FRAME_AT` onward, measured at a sustained **29.9 rendered frames/s** (the guest's own rate) with **15 ms/frame** in `RenderFrame`. Getting there was measurement-led — phase timers showed a 296 ms frame was 47 ms setup, 218 ms draw loop (116 translation, 21 pipelines, 7 textures, ~74 per-draw buffer churn), **6 ms of actual GPU work** and 25 ms readback. `RendererPersistent` now holds the device objects, render target, passes, layouts, shader translations, pipelines, textures, samplers, command pool, fence, descriptor pool (reset per frame) and readback buffer; per-draw uniform blocks and index buffers come from one persistently-mapped **arena** sized to the previous frame's high-water mark (with a standalone-buffer overflow fallback); the readback buffer is **HOST_CACHED** (it was 15 ms for 3.7 MiB at write-combined speed); and the ~40 ms per-pixel census belongs to a capture (`FrameDrawInputs::report`), not to every frame. Result: frame 1 ~139 ms, frames 2+ **15-23 ms**. `GEARS_DRAW=1` fires the one-shot hot-pair draw (see `re_frontier.py show draw-backend`). **`GEARS_DRAW_FRAME=1` renders a WHOLE frame**, and it now renders a real **post-load scene** frame, not just the loading frame: every `DRAW_INDX`/`_2` is captured with the register-file state live at that draw, each distinct shader translated+cached by hash, **texture descriptor set layouts built per shader** from the translated shader's own binding list (N images at bindings 0..N-1, then M samplers — a fixed layout is undefined behaviour and hard-crashed the RADV compiler), pipelines cached per (vs,ps,prim,**output-merger state**), and all draws issued **in submission order** into one persistent colour+depth target → `scratch/screenshots/frame.ppm`. The **output-merger state is the guest's own**: `RB_COLOR_MASK` (0x2104), `RB_BLENDCONTROL0` (0x2201), `RB_DEPTHCONTROL` (0x2200) are read per draw and baked into the pipeline — without this the frame's depth-only passes (colour mask 0) painted everything black. Guest 16-bit index buffers are widened to 32-bit on the way in (they were previously undersized while bound as `UINT32`). **Measured headless on scene frame 600**: 173–176 of 173–176 draws issued, **0 skipped**, 22 shader pairs / 26 shaders / 28 pipelines / 6 texture layouts / 5 pipeline layouts, 224 texture bindings; prims `point_list`×48 `triangle_list`×93 `rectangle_list`×19 `quad_list`×16. **Viewport and scissor are the guest's own, per draw** (`draw_util::GetHostViewportInfo`/`GetScissor`, Vulkan dynamic state — 13 distinct combinations on frame 600, including 48 draws scissored to 16×16). **`kQuadList` is expanded to a triangle list** (0,1,2 / 0,2,3 per group of 4) as Xenia's `PrimitiveProcessor` does; it previously fell through to `TRIANGLE_LIST` and regrouped the vertices into unrelated triangles, and this frame's *entire world geometry* is `quad_list`. **`kRectangleList` is expanded by a geometry shader** ported from Xenia's `VulkanPipelineCache::GetGeometryShader` (`draw::BuildRectangleGeometryShader`): the guest gives three vertices and the hardware infers the fourth by mirroring one across the longest edge, and every attribute is mirrored the same way — so it cannot be synthesized in the index buffer ahead of the vertex shader. 19 of 19 rectangle draws expanded, 2 distinct geometry shaders. This removed **both** the hard diagonal split and the pink/red tint, which were **one defect**: the rectangles are the frame's full-screen colour-grade passes, so half of every one was going ungraded. The pipeline cache is keyed on the **module handles**, not the microcode hashes — a hash stopped identifying a stage once one microcode began translating to several shaders. **The frame now renders the game**: 919796–921600 of 921600 px non-black (99.8–100.0%) across 4 runs, against 0 in every run before — `scratch/screenshots/rect/frame.png` is the real title screen (logo, PRESS START, copyright line, wall art), with no diagonal and no tint. The fix was the **shader modification**: a Xenos translation is not a function of the microcode alone — Xenia's `Modification` carries the **interpolator mask** the VS/PS pair exchanges, derived from the pair plus `SQ_PROGRAM_CNTL`/`SQ_CONTEXT_MISC`. Both stages were translated with the *default* modification, whose mask is 0, so every vertex shader exported no interpolators and every pixel shader read them as zero — geometry rasterised perfectly and shaded pure black. `DeriveShaderModifications` now ports `GetCurrentVertex/PixelShaderModification` in full and the shader cache is keyed by **(hash, modification)**. **RT link (`GEARS_DRAW_RT=1`)**: resolve destinations are read from `RB_COPY_DEST_BASE`, a texture binding whose fetch-constant base matches one is bound to the rendered colour target, and the render pass is split at each such draw (copy colour → `rtSample`, resume with `LOAD`). Measured: 8 resolve destinations, **30 of 224 texture bindings (13.4%) served by the rendered RT**, 10 segments, 9 snapshots. Knobs: `GEARS_DRAW_STATS=1` (per-draw pipeline statistics — vertices, primitives in, primitives after clip+cull, fragment invocations; this is what separated *not rasterising* from *shading black*), `GEARS_DRAW_ONLY=N` (emit only draw N over the clear), `GEARS_DRAW_VDUMP=N` (dump draw N's first vertices from the mirror at the shader's own stride), `GEARS_DRAW_NOBLEND=1`, `GEARS_DRAW_NOTEX=1`, `GEARS_DRAW_FIXEDVP=1`, `GEARS_DRAW_NODEPTH=1`, `GEARS_DRAW_DEPTH_CLEAR=<float>`, `GEARS_DRAW_TEX_DUMP=1` are **control arms and dumps, never fixes**; `GEARS_DRAW_FRAME_AT=N` picks the frame (loading frames carry 2–3 draws; the scene phase starts at frame **571** and holds **168–186** draws/frame), `GEARS_DRAW_FRAME_STEP=N` writes a checkpoint image every N draws, `GEARS_DRAW_FRAME_LIST=1` logs a per-draw census, `GEARS_DRAW_VALIDATE=1` Vulkan validation, `GEARS_DRAW_CENSUS=1` the per-run draw census. The one-shot render blocks the command-processor thread for ~520 ms on a 173-draw frame and does not perturb the guest (issue #26) |
| **Guest textures** | `runtime/gpu_draw_xlate.cpp` (`DecodeGuestTexture`, `DeriveSamplerState`), `runtime/gpu_draw.cpp` (`uploadTexture`, `getSampler`) | **real, verified on real data** | Texture fetch constants (`0x4800 + fc*6`) are decoded into host images using **Xenia's own machinery** — `texture_util::GetSubresourcesFromFetchConstant` (extents, mip range), `GetGuestTextureLayout` (row pitch, slice strides), `texture_address::Tiled2D/Tiled3D` (the tiled address function), `FormatInfo` (block size). Endianness is an XOR on the source byte offset (`k8in16`→`^1`, `k8in32`→`^3`, `k16in32`→`^2`); the guest swizzle is composed with the host format's component order per `TextureCache::GuestToHostSwizzle` and applied through the `VkImageView` component mapping. Sampler state is the guest's too (filters, clamp modes, anisotropy from the fetch constant each sampler binding names). **Measured on scene frame 600**: 26 distinct fetch constants, 20 uploaded (1.5 MiB), **176 of 218 texture bindings (80.7%) served by real guest data**; format distribution `k_DXT4_5`×13, `k_DXT1`×7, `k_24_8_FLOAT`×2, `k_16_16`, `k_16_16_16_16_EXPAND`, `k_16_16_16_16_FLOAT`, `k_8_8_8_8` — all 2D, all tiled. **Decode verified independently**: `GEARS_DRAW_TEX_DUMP=1` + `tools/decode_bc.py` produce recognisable Gears world art from the decoded blobs. Gaps: base level only (no mip tails), no cross-frame cache, `k_24_8_FLOAT`/`k_16_16_16_16_EXPAND` unmapped and counted |
| **Shader translation** | `xenia_gpu/`, `tools/xenos_translate/` | **real, offline only** | Xenia's translator builds and runs in our tree. 425 of 425 offline containers, and **38 of 38 shaders the running title actually binds**, translate and pass `spirv-val`. Now also driven **at runtime** by the guest-draw backend. Details in `docs/d3d-seam.md` §3 |
| **Shader capture** | `runtime/vd_null_gpu.cpp` (PM4 `IM_LOAD`/`IM_LOAD_IMMEDIATE`), `runtime/hle_d3d.cpp` (API argument scan) | **real** | `GEARS_SHADER_CAPTURE=1` dumps every microcode payload the GPU is handed, with bind counts, to `GEARS_SHADER_CAPTURE_DIR`. `GEARS_SHADER_ARGSCAN=1` probes 48 D3D entry points for shader arguments — how `SetVertexShader`/`SetPixelShader` were identified |
| **System constants UBO** | `tools/system_constants/`, `runtime/vd_null_gpu.cpp` (register snapshot) | **real, verified byte-exact** | Ports Xenia's `SystemConstants` + `UpdateSystemConstantValues` (non-FSI) from our tracked register file to the 528-byte `xe_uniform_system_constants` UBO. Layout matches the bound shader's SPIR-V member offsets exactly; NDC scale/offset (Xenia `draw_util`) consistent with a 1280×720 target. Texture-cache fields stubbed visibly. **Now wired into the guest-draw backend** and ported into `runtime/gpu_draw_xlate.cpp`; VGT_DRAW_INITIATOR is live (mirrored from DRAW_INDX) so prim-type/index-size are no longer stubbed |
| **Audio** | `xaudio_null.cpp` | **null** | Accepts frames, plays nothing. Its callback never fires |
| Input | — | **absent** | |
| Networking | — | **absent** | 32 `Net*` imports |
| User/content | — | **absent** | 49 `Xam*` imports |

## Import coverage

~102 of 226 implemented. Every unimplemented import **aborts with its name and
argument registers** on first call — there are no silent stubs, so the next gap
is always named.

`implemented_imports.h` is the single list that decides which generated stub is
suppressed; adding an implementation means adding its name there.

## Where is X?

- *Why does the game see zeros at an address?* → `import_variables.cpp` (variable imports start zeroed)
- *Why is a physical pointer valid at four addresses?* → `guest_memory.cpp`, `MapPhysicalAliases`
- *What decides an import traps vs runs?* → `implemented_imports.h` + `tools/gen_import_stubs.py`
- *Why does nothing render?* → `vd_null_gpu.cpp` executes the command stream but performs no draws in the normal path. `GEARS_DRAW_FRAME=1` runs the whole-frame guest-draw backend (`runtime/gpu_draw.cpp`), which issues **every** draw of a selected frame and now produces a recognisable game image (99.8–100% non-black). General rendering is the HLE work in `docs/d3d-seam.md`
- *Why is a shader's output black even though it rasterises?* → the **shader modification**, not the shader. `Modification.interpolator_mask` decides which interpolators the VS exports and the PS reads; a zero mask (which `GetDefault*ShaderModification` gives you) makes every interpolant read as 0 while position and clipping stay perfect. Derive it per draw from the pair + `SQ_PROGRAM_CNTL`/`SQ_CONTEXT_MISC` (`DeriveShaderModifications`), and cache translations by **(hash, modification)**. Confirm with `GEARS_DRAW_STATS=1`: many fragment invocations + no pixels = shading, not rasterising
- *Which frame should I capture?* → `GEARS_DRAW_FRAME_AT=N`. Loading frames carry 2–3 draws; the scene phase begins at frame **571** and holds 168–186 draws/frame. Profile any run by setting `GEARS_DRAW_FRAME_AT` beyond its length — one `guest-draw: frame N has M draws` line per frame
- *Why is a draw black / why did the driver crash on a shader?* → the output-merger state (`RB_COLOR_MASK`/`RB_BLENDCONTROL0`/`RB_DEPTHCONTROL`) and the texture descriptor set layout are **per draw and per shader**; hardcoding either is a defect, not a simplification. Bisect with `GEARS_DRAW_FRAME_STEP=N` (checkpoint images) + `GEARS_DRAW_FRAME_LIST=1` (per-draw census)
- *Why is the frame rate what it is?* → vblank pacing is faithful (~8 ms/wait on `0x30B004`) and must not be shortened; see `catalog.py show 16`
- *Where do I get Xenos shader/packet semantics?* → `extern/xenia` submodule (BSD-3 fork, pinned); see `docs/xenia-reuse.md`
- *How do I get a shader out of the game and into SPIR-V?* → `tools/shader_extract.py` then `scratch/build/tools/xenos_translate/xenos_translate`; layout in `docs/d3d-seam.md` §3
- *Which shaders does the title actually bind, and where?* → `GEARS_SHADER_CAPTURE=1` (PM4 sequencer loads), then `xenos_translate --raw` and `tools/compare_bound_shaders.py`; setters are `sub_82222B98` (vertex) / `sub_82222808` (pixel), see `catalog.py show 21`
- *Why doesn't captured microcode match the corpus?* → the title patches the vertex fetch into the instruction at bind time; `catalog.py show 22`
- *Why are Xenia's asserts off in `xenia_gpu/`?* → every vertex shader in this title has a zero vfetch stride; see the comment in `xenia_gpu/CMakeLists.txt`
- *What has already been ruled out for the current crash?* → `catalog.py show 1`
- *Why won't my gdb watchpoint fire?* → `catalog.py show 5` (physical aliasing / stale addresses)
- *Why do registers look wrong at a watchpoint?* → `catalog.py show 6`
- *Why is my native override never entered?* → clang folds intra-TU calls through the weak alias; run `tools/prepare_overrides.py`, and note `sub_X` is C++-mangled, not `extern "C"`. Details in `catalog.py show 16`

## Current state

Boots, loads its own packages, plays the startup movies, and reaches scene
rendering at **~30 fps sustained**. The normal path still draws nothing (the
command processor executes the stream but performs no draws); the one-shot
whole-frame guest-draw backend (`GEARS_DRAW_FRAME=1`) now renders a captured
frame into a **recognisable game image** — see the guest-draw backend row.

The heap leak that capped the run at roughly 160 seconds of gameplay (frame
~4800) is FIXED -- see issue #17. `GuestHeap` now recycles freed address space
through a coalescing free list; peak use plateaus at 192 MiB (title) and 194 MiB
(physical) of the 512 MiB windows and stays flat, and the title runs past frame
25000 at an unchanged ~30 fps.

Note the heap that exhausted was the **title** heap (0x40000000), not the
physical heap, and its genuinely-live set at exhaustion was only 67.9 MiB -- the
rest was leak.

That intermittent SIGSEGV at frame ~21840 is now DIAGNOSED and attributed to the
heap after all -- see issue #18. `GuestMemory::Commit` never zero-filled, which
was invisible while the heap only moved forwards (fresh `mmap` pages are already
zero) and became fatal once address space was recycled, because `Free` keeps the
pages committed on purpose. The title's `RtlHeap` read a stale block header out
of a segment extension it had just committed. `GuestHeap` now zeroes exactly the
free -> allocated transitions, and only below the ever-allocated high-water mark
so untouched space costs nothing. The same core also exposed three pairs of
overlapping live regions produced by the reservation-growth path; it now absorbs
what it grows over instead of leaving a second owner.

Next limit is unknown. One 900 s run after the fix reached 26160 frames at
~30 fps with no core and no heap errors -- consistent with the fix, but one run
against an intermittent fault is not on its own proof.

## Standing hazards

Several of these have each cost more than one session:

- **Decompiler output is fiction** wherever Ghidra failed to rebuild a function,
  because `DecompXbox.py` stubs the save/restore helper ranges -- and `Disasm.py`
  silently degrades to a byte dump in exactly those places. Check anything
  suspicious with `tools/ppcdis.py`, which decodes from the image directly.
  Five wrong conclusions have come from this, including two "established facts".
- **A negative result is only as good as the detector.** Ring lapping was
  recorded as ruled out twice on the strength of a check that could not fire,
  because the available-space calculation was masked. Before trusting a prior
  negative, confirm the test could have detected the thing.
- **Suspect this port before the title.** Most blockers so far have been defects
  here, not in the game: silently dropped switch cases, a 64-bit switch on a
  32-bit dispatch, an unlocked heap, an import-name comparison that never
  matched, a device register written in the wrong byte order, alias folding that
  made overrides no-ops, and a missing predicate bit that cost 70x frame rate.

A differential harness against Xenia was tried and **abandoned** -- it never
executed the title, and an unreliable oracle is worse than none. See
`catalog.py show 7`. Do not resume it. This is unrelated to `extern/xenia`,
which is used as a *reference for hardware contracts* and has been good for
exactly that.

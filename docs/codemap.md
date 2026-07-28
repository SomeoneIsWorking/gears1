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
| `tools/frame_replay/` + `runtime/frame_capture.cpp` | **The renderer's instrument — reach for this before any renderer hypothesis.** `GEARS_DRAW_FRAME_DUMP=<path>` records one frame's whole draw stream (per-draw register snapshots, deduplicated microcode, the non-zero guest pages); `frame_replay <capture>` re-renders it offline in **~550 ms with no guest**. Every `GEARS_DRAW_*` knob works, so two arms of a comparison run on **byte-identical input** — which a live run cannot give you, because reaching a gameplay frame costs a 200 s scripted menu walk and each walk lands on a *different* game moment. Validated both ways: byte-identical to the live render of the same frame, and it *changes* when the input does |
| `tools/capture_gameplay_frame.sh` | The scripted controller walk from the title screen into Act 1 gameplay (~743 draws, the deferred UE3 pipeline) rather than the title screen (~170 draws, one surface). Use it once to get a capture, then iterate with `frame_replay` |
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
| **Guest-draw backend** | `runtime/gpu_draw.cpp` (Vulkan renderer), `runtime/gpu_draw_xlate.cpp` (Xenos→SPIR-V + Modification + SystemConstants + the resolve compute shaders, isolated static lib), `runtime/frame_capture.cpp` | **real, verified on real data** | **Renders the guest's own frames, live, including the deferred in-game scene.** Persistent renderer (`RendererPersistent`) driving ~30 fps; per-draw shader **Modification** (the interpolator mask — a zero mask shades every pixel black while rasterising perfectly), guest textures from fetch constants, guest viewport/scissor/output-merger state, `kQuadList` expansion and a `kRectangleList` **geometry shader**. **Render-target cache**: one host colour target per EDRAM `RB_COLOR_INFO` base, every draw routed to its surface; `RB_MODECONTROL.edram_mode` honoured (`kCopy` is a RESOLVE, not geometry; the pixel shader runs **only** for `kColorDepth`). **Guest memory**: the SSBO spans the full 512 MiB physical window — it was 64 MiB, and an Act 1 frame fetches to 237 MiB, so 606 of 722 draws read ZERO and every primitive collapsed at clipping (`catalog.py show 30`) — while uploading only the ranges the frame fetches (764 KiB in 25 spans). **Clears are the guest's**: they ride on copy draws, so depth is cleared once per predicated TILE with `RB_DEPTH_CLEAR` decoded per depth format (`catalog.py show 31`, `34`). **Tile assembly**: a resolve destination is a REGION OF A TEXTURE — the guest folds a tile's row offset into `RB_COPY_DEST_BASE`, so `0xc2e0000` is row 512 of the texture at `0xbde0000` (`catalog.py show 32`). **Resolves are compute dispatches, not blits**: colour applies `copy_dest_exp_bias` (this title resolves HDR with bias −3, so ignoring it left the tonemap's input 8× too bright — `catalog.py show 33`) and `copy_dest_swap`; depth writes what the guest's `k_24_8_FLOAT` fetch would produce into an R32_SFLOAT target (`catalog.py show 35`). **Measured on a captured Act 1 frame**: 722/737 draws issued, 0 skipped; 12 colour + 3 depth resolves; 54 bindings served by a resolve target, **0 by a stub**; 0.0% of pixels saturated. Knobs are **control arms and dumps, never fixes**: `GEARS_DRAW_STATS`, `GEARS_DRAW_DIAG=<tsv>` (the per-draw verdict table), `GEARS_DRAW_RESOLVE_DUMP`, `GEARS_DRAW_RESOLVE_COMPUTE`/`_BLIT`/`_SCALE`/`_NOSWAP`, `GEARS_DRAW_ONLY[_BASE]`, `GEARS_DRAW_VDUMP=N` (indexed as the diag table's `draw` column), `GEARS_DRAW_NOTEX`, `GEARS_DRAW_NOBLEND`, `GEARS_DRAW_NODEPTH`, `GEARS_DRAW_DEPTH_CLEAR`, `GEARS_DRAW_VALIDATE`, `GEARS_DRAW_FRAME_DUMP=<path>` (frame capture) |
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

- *A draw's geometry vanished before rasterisation?* → check the **frame geometry reach** line first. A vertex fetch past the guest-memory SSBO mirror reads **zero**, so every vertex collapses to the origin and the whole primitive dies at clipping — which looks nothing like a memory bug and exactly like a transform bug. This cost the project the entire in-game world: the mirror was 64 MiB while an Act 1 frame fetches to 237 MiB (`catalog.py show 30`). The mirror now spans the full 512 MiB window and uploads only the ranges the frame fetches
- *Why does the game see zeros at an address?* → `import_variables.cpp` (variable imports start zeroed)
- *Why is a physical pointer valid at four addresses?* → `guest_memory.cpp`, `MapPhysicalAliases`
- *What decides an import traps vs runs?* → `implemented_imports.h` + `tools/gen_import_stubs.py`
- *Why does nothing render?* → `vd_null_gpu.cpp` executes the command stream but performs no draws in the normal path. `GEARS_DRAW_FRAME=1` runs the whole-frame guest-draw backend (`runtime/gpu_draw.cpp`), which issues **every** draw of a selected frame and now produces a recognisable game image (99.8–100% non-black). General rendering is the HLE work in `docs/d3d-seam.md`
- *Why is a shader's output black even though it rasterises?* → the **shader modification**, not the shader. `Modification.interpolator_mask` decides which interpolators the VS exports and the PS reads; a zero mask (which `GetDefault*ShaderModification` gives you) makes every interpolant read as 0 while position and clipping stay perfect. Derive it per draw from the pair + `SQ_PROGRAM_CNTL`/`SQ_CONTEXT_MISC` (`DeriveShaderModifications`), and cache translations by **(hash, modification)**. Confirm with `GEARS_DRAW_STATS=1`: many fragment invocations + no pixels = shading, not rasterising
- *A draw contributed nothing — which stage did it die at?* → **do not guess, and do not A/B against a live run.** `GEARS_DRAW_DIAG=<path.tsv>` writes one row per issued draw joining what it *was* (EDRAM surface, `edram_mode`, primitive, shader hashes) with what it *did* (pipeline statistics: IA vertices, IA primitives, primitives after clip+cull, fragment invocations) and every piece of state that can silently zero it (depth test/write/func, colour mask, blend, viewport and scissor extents, `PA_CL_CLIP_CNTL`, `PA_SU_SC_MODE_CNTL`, `PA_CL_VTE_CNTL`, `PA_SC_WINDOW_OFFSET`, the raw viewport scale/offset). The `verdict` column names the stage directly: `no_primitive_assembled` / `killed_by_clip_or_cull` / `rasterised_no_fragment` / `depth_only_no_colour` / `colour_fully_masked` / `shaded`. Run it under `frame_replay` so it costs a second — this is how issue #30 and #31 were separated, after two earlier suspects had survived precisely because live runs were not comparable
- *How do I iterate on the renderer without a 200-second boot?* → capture once with `GEARS_DRAW_FRAME_DUMP=<path>` on a `tools/capture_gameplay_frame.sh` run, then replay with `scratch/build/runtime/frame_replay <capture>` as often as needed. See the tools table
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

Boots, loads its own packages, plays the startup movies, walks its own menus
under a controller, reaches **Act 1 gameplay**, and renders the guest's frames at
~30 fps. The deferred in-game frame renders: whole (both predicated tiles),
correctly exposed (0.0% of pixels saturated), depth-tested and depth-lit.

**Not yet faithful**, each measured rather than guessed and tracked on
`docs/re-frontier.md`: the colour clear is still a diagnostic sentinel rather
than the guest's; the resolve ignores destination tiling and scaling; cull mode
is host-fixed to NONE; resolved depth is not quantised to the guest's float24
(one 20-bit ULP); and one host depth image serves four `RB_DEPTH_INFO` bases —
measured as ~1% of draws, since both major surfaces genuinely share base 0x0.

**The renderer is debugged through instruments, not screenshots**, and that is
not a stylistic preference. Six defects this session produced output that looked
plausible: a descriptor pool sized from a list that was still empty, a dispatch
borrowing descriptor sets of the wrong layout, a pool undersized for two set
types, an `OpImageFetch` missing its `Lod` operand, a `VDUMP` index that meant a
different draw from the diagnostic table's, and a census recording the previous
draw's shader hash. Each was caught by a reading that could not have come from a
healthy system — most often a resolve target reading exactly `0.000`. **The dump
that catches them had itself to be fixed first**: it read every target as
half-float and so reported `0.000` for an R32_SFLOAT depth target regardless of
content. An instrument that cannot tell "wrote nothing" from "I am reading it
wrong" is worse than none.

The heap leak that capped runs at ~160 s is fixed (issue #17), as is the
intermittent SIGSEGV at frame ~21840 (issue #18): peak use plateaus at 192/194
MiB and the title runs past frame 25000 at ~30 fps.

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

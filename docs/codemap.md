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
| **HLE D3D** | `hle_d3d.cpp` | **partial** | The native-override seam for the guest D3D layer, with a per-frame call census carrying call-site provenance (channel `hle`). Overriding **works** now -- it did not before `tools/prepare_overrides.py`, and a strong override linked cleanly while never being entered. ~60 guest functions are wrapped so far, all for instrumentation (census, worker-queue watchpoint, shader argument scan); none replaces guest behaviour |
| **GPU** | `vd_null_gpu.cpp` | **command processor, renders nothing** | Consumes the ring, follows indirect buffers, and executes TYPE0 register writes, `EVENT_WRITE_SHD` fences, `EVENT_WRITE_ZPD` occlusion reports, `WAIT_REG_MEM`, `INTERRUPT` (dispatched per CPU) and **predication** via bin mask/select. No draw is ever performed and no pixel is produced. Vblank at 60 Hz (`GEARS_NO_VBLANK=1` disables). `GEARS_CP_STALL_MS=N` blocks the CP thread for N ms at the first swap doing nothing else — the control arm for "did our host-side work perturb the guest?"; measured, a 20 s stall costs throughput only and induces no guest error path (issue #26) |
| **Guest-draw backend** | `runtime/gpu_draw.cpp` (Vulkan renderer), `runtime/gpu_draw_xlate.cpp` (Xenos→SPIR-V + SystemConstants, isolated static lib) | **real, re-partial** | `GEARS_DRAW=1` fires the one-shot hot-pair draw (see `re_frontier.py show draw-backend`). **`GEARS_DRAW_FRAME=1` renders a WHOLE frame**, and it now renders a real **post-load scene** frame, not just the loading frame: every `DRAW_INDX`/`_2` is captured with the register-file state live at that draw, each distinct shader translated+cached by hash, **texture descriptor set layouts built per shader** from the translated shader's own binding list (N images at bindings 0..N-1, then M samplers — a fixed layout is undefined behaviour and hard-crashed the RADV compiler), pipelines cached per (vs,ps,prim,**output-merger state**), and all draws issued **in submission order** into one persistent colour+depth target → `scratch/screenshots/frame.ppm`. The **output-merger state is the guest's own**: `RB_COLOR_MASK` (0x2104), `RB_BLENDCONTROL0` (0x2201), `RB_DEPTHCONTROL` (0x2200) are read per draw and baked into the pipeline — without this the frame's depth-only passes (colour mask 0) painted everything black. Guest 16-bit index buffers are widened to 32-bit on the way in (they were previously undersized while bound as `UINT32`). **Measured headless on scene frame 600**: 173–176 of 173–176 draws issued, **0 skipped**, 22 shader pairs / 26 shaders / 28 pipelines / 6 texture layouts / 5 pipeline layouts, 224 texture bindings; prims `point_list`×48 `triangle_list`×93 `rectangle_list`×19 `quad_list`×16. **Output is still black**: every texture is a 1×1 stub, so textured scene draws shade black — texture upload is the next real step, not the RT link. **RT link (`GEARS_DRAW_RT=1`)**: resolve destinations are read from `RB_COPY_DEST_BASE`, a texture binding whose fetch-constant base matches one is bound to the rendered colour target, and the render pass is split at each such draw (copy colour → `rtSample`, resume with `LOAD`). Measured: 8 resolve destinations, **30 of 224 texture bindings (13.4%) served by the rendered RT**, 10 segments, 9 snapshots. Knobs: `GEARS_DRAW_FRAME_AT=N` picks the frame (loading frames carry 2–3 draws; the scene phase starts at frame **571** and holds **168–186** draws/frame), `GEARS_DRAW_FRAME_STEP=N` writes a checkpoint image every N draws, `GEARS_DRAW_FRAME_LIST=1` logs a per-draw census, `GEARS_DRAW_VALIDATE=1` Vulkan validation, `GEARS_DRAW_CENSUS=1` the per-run draw census. The one-shot render blocks the command-processor thread for ~520 ms on a 173-draw frame and does not perturb the guest (issue #26) |
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
- *Why does nothing render?* → `vd_null_gpu.cpp` executes the command stream but performs no draws in the normal path. `GEARS_DRAW_FRAME=1` runs the whole-frame guest-draw backend (`runtime/gpu_draw.cpp`), which issues **every** draw of a selected frame — including a real post-load scene frame, 176/176 draws, 0 skipped. It is still **black because every texture is a 1×1 stub**, not because of the render target: the RT link is implemented and measured (13.4% of texture bindings). Texture upload is the next real step. General rendering is the HLE work in `docs/d3d-seam.md`
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
rendering at **~30 fps sustained** -- while drawing nothing, because the command
processor executes the stream but performs no draws.

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

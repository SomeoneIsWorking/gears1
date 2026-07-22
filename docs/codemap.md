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
| `tools/gen_import_stubs.py` | Emits a trapping stub for every import not named in `implemented_imports.h` |
| `extern/XenonRecomp` | Submodule → our fork, branch `gears` |
| `runtime/` | The PC-side runtime. See below |
| `tests/` | `test_vmx_instructions` (fork's instruction implementations) and `test_runtime_logic` (kernel object semantics, path translation). Both mutation-checked |
| `tools/catalog.py` + `docs/issues/` | **Findings registry, keyed by symptom. Search it before investigating anything.** `catalog.py search "<symptom>"` |
| `debug_journal/` | Dated narrative of each session. The catalog is the index into it |
| `scratch/` | All derived output, gitignored: `ppc/` generated C++, `game/` extracted disc, `bin/`, `logs/`, `raw/` |

## Runtime subsystems

| Area | Files | Status | Notes |
|---|---|---|---|
| Guest memory | `guest_memory.*` | **real** | 4 GiB sparse window. Physical RAM is one `memfd` aliased at `0x0/0xA0/0xC0/0xE0000000` because guest code converts between them by masking the top 3 bits |
| Heaps | `guest_heap.*` | **partial** | Bump allocator, page-granular, honours requested alignment. **Never reuses freed memory** — deliberate, but a leak |
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
| **GPU** | `vd_null_gpu.cpp` | **null** | Tracks driver state, retires command buffers **without executing them**. No command processor. Register file at `0x7FC00000` is inert memory. Vblank interrupt is driven at 60 Hz (`GEARS_NO_VBLANK=1` disables) |
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
- *Why does nothing render?* → `vd_null_gpu.cpp`, top-of-file comment
- *What has already been ruled out for the current crash?* → `catalog.py show 1`
- *Why won't my gdb watchpoint fire?* → `catalog.py show 5` (physical aliasing / stale addresses)
- *Why do registers look wrong at a watchpoint?* → `catalog.py show 6`

## Current blocker

Boot dies in `sub_82766F68` dereferencing `1.0f` as a pointer. Fully traced: a
float store lands on a zero-initialised list head, the list code faithfully
propagates it, the walker dereferences it. **Twelve mechanisms eliminated** —
see the journal, and do not re-investigate them.

Recommended next method is **static reverse engineering** of the two
conflicting functions: determine from the binary what `sub_82761CA8` believes
it is writing at `+36`, and what `sub_82766F68`'s walker believes lives there.

A differential harness against Xenia was tried and **abandoned** — it never
executed the title across many attempts, and an unreliable oracle is worse than
none because any divergence is ambiguous between its bug and ours. See
`catalog.py show 7`. Do not resume it.

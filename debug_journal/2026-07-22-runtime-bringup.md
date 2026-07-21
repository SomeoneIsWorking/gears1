# 2026-07-22 — Runtime bring-up: the game executes

Goal: get from "the C++ is generated" to "guest code actually runs", fixing
whatever came up.

## Result

Guest code executes. Boot now reaches the kernel **object manager**
(`NtCreateEvent`) — roughly a dozen distinct kernel subsystems deep into Gears'
startup path — with **20 of 226** imports implemented. Everything not yet
implemented aborts loudly rather than returning a plausible lie.

## Does the generated code compile?

Yes, all **193** translation units, unmodified, with Clang 22. One TU takes
~1.4 s at `-O0`. That was the first open question and it is settled.

One real bug found: my own `mulhdu` emission produced
`unsigned __int128(x)`, which is not a valid functional cast. Fixed in the fork
to `__uint128_t(x)` / `__int128_t(x)`.

## The runtime

`runtime/` is new. It reserves 2.06 GiB of guest address space (the 4 GiB guest
window is sparse; only the image, heap and function table are committed), maps
the XEX with XenonUtils, installs **49,475** functions into the indirect-call
table, and calls `_xstart`.

## Bugs found and root-caused, in order

### 1. Data imports were never resolved (upstream bug)

**Symptom:** SIGSEGV three guest frames in, at
`lwz r11, 0(r11)` after `lwz r11, 2200(r11)` — i.e. dereferencing a pointer
loaded from `0x82000898`. The value there was `0x93010100`, not an address.

**Root cause:** `XenonUtils/xex.cpp` walks the XEX import table and, for
*function* imports (`type != 0`), overwrites the thunk with a `nop/nop/nop/blr`
stub and registers a symbol. For *variable* imports (`type == 0`) it does
nothing but byte-swap the slot in place — so the slot keeps holding its raw
ordinal record. Decoding `0x00010193`: `type=0`, `ordinal=0x193`, which
`xboxkrnl_table.inc` names `XexExecutableModuleHandle`, a `kVariable` export.
The game loaded that record and dereferenced it.

This is not Gears-specific; any title reading an imported variable hits it.

**Fix:** XenonUtils now records variable imports on `Image::importVariables`
(name, library, thunk address, ordinal) instead of dropping them, and the
runtime points each thunk slot at real guest storage. **236** variable imports
resolved, all with known ordinals.

Their contents start zeroed. Where zero is not a valid initial value the game
will misbehave and say so — preferable to inventing values now.

### 2. No guest thread block (r13 unset)

**Symptom:** SIGSEGV at `PPC_LOAD_U32(ctx.r13.u32 + 256)`.

**Root cause:** on the Xbox 360, `r13` holds the address of the current
thread's KPCR, and offset `0x100` is `current_thread`. The console kernel sets
this up before a title runs; we started the guest with `r13 = 0`.

**Fix:** `runtime/guest_thread.cpp` allocates a KPCR, a thread block, a TLS
block and the stack, populates `tls_ptr`, `stack_base`, `stack_limit` and
`current_thread`, and boots with `r13` pointing at the KPCR. Only the fields
the game actually reads are populated; the rest stays zeroed so an unexpected
read shows up as zero rather than as plausible noise.

## Implemented so far (20/226)

- **Memory** — `NtAllocateVirtualMemory`, `NtFreeVirtualMemory` over a
  page-granular heap at `0x40000000` (512 MiB), honouring `MEM_LARGE_PAGES`
  for 64 KiB alignment. Verified: the game's first allocation returns
  `0x40000000` and it proceeds.
- **Pool** — `ExAllocatePool{,WithTag}`, `ExFreePool`.
- **Critical sections** — backed by real host `std::recursive_mutex` keyed by
  guest address, with the guest-visible `lock_count`/`recursion_count` fields
  maintained. Deliberately not a single-threaded fake, so it stays correct once
  guest threads exist.
- **TLS** — `KeTlsAlloc/Free/Get/SetValue`, `thread_local` backed.
- **Time** — `KeQueryPerformanceFrequency` (50 MHz Xenon timebase),
  `KeQuerySystemTime`.
- **Misc** — `KeGetCurrentProcessType`, `XexCheckExecutablePrivilege`,
  `DbgPrint`, `FscSetCacheElementCount`.

`GuestHeap::Free` deliberately leaves pages committed: the guest may hold
aliases, and decommitting would turn a use-after-free into a host crash far
from its cause.

## Honest status

- Nothing is rendered. There is no graphics, audio, input or file I/O yet.
- 206 imports remain unimplemented; each aborts with its name and argument
  registers when first called.
- The **memory-barrier debt from the previous entry is still unpaid** and
  becomes load-bearing the moment `ExCreateThread` is implemented, because
  `sync`/`lwsync`/`eieio`/`isync` are still no-ops.
- The **1,394 jump-table/function-boundary errors are still unaddressed.** None
  has been hit yet, which only means startup has not reached that code.

## Next

1. Kernel object manager: handle table, `NtCreateEvent`, `NtWaitFor*`,
   mutants, semaphores. This is the gateway to `ExCreateThread`.
2. Pay the memory-barrier debt *before* threads, not after.
3. File I/O (`NtCreateFile`/`NtReadFile`) so the title can load its packages.

---

## Continued: barriers, threading, graphics init

Picked up in the order the previous entry recommended — barriers *before*
threads, not after.

### Memory barriers paid off first

`sync`, `lwsync`, `eieio` and `isync` now emit real `__atomic_thread_fence`:
acq_rel for lwsync/eieio, acquire for isync, seq_cst for sync. In Gears that is
**121 acq_rel and 12 seq_cst sites**. The 12 are the ones that mattered — `sync`
exists for StoreLoad ordering, which x86-64's TSO does *not* provide, so those
were genuinely broken before. `isync` had no case at all and would have been
reported as an unrecognized instruction had any title used one.

### Guest threading works

`ExCreateThread` spawns a host thread with its own `PPCContext`, KPCR, TLS and
stack. Nothing else was needed — the recompiled code takes its context by
parameter, so per-thread state falls out for free. `CREATE_SUSPENDED` is a real
gate the thread waits on.

**Verified:** two guest threads start and run concurrently with the main
thread, deterministically across three consecutive runs.

`KeSetAffinityThread` is deliberately **not honoured** — the title pins work to
specific Xenon hardware threads, which is meaningless on a host with different
topology. It records and reports back the mask and lets the host scheduler
place the thread. Same for `KeSetBasePriorityThread`.

### Kernel object manager

A handle table plus one waitable object type covering notification and
synchronisation events. Handles start at `0xF8000000` so one mistaken for a
pointer faults instead of landing in real guest memory. Objects get a
guest-visible address lazily, only when something asks for one via
`ObReferenceObjectByHandle`, so the common handle-only path costs nothing.

`ObDereferenceObject` is a no-op: host objects are `shared_ptr`-owned and
outlive the guest's references. Guest lifetime bugs therefore will not
reproduce here — a difference worth remembering, not a behaviour to rely on.

### Graphics initialisation reached

`XGetVideoMode` reports 1280x720 progressive widescreen at 60 Hz, and physical
memory now allocates from a separate heap at `0xA0000000` because those
addresses are handed to the GPU and must not come from the title heap. This
also forced the guest reservation up from 2.06 GiB to the full 4 GiB window.

Boot now stops at **`VdInitializeEngines`** — the Xenos video driver. That is
the door to the command ring buffer, and past it nothing works without an
actual GPU backend.

### Status: 43/226 imports

Still true: nothing is rendered, no file I/O, no audio, no input. The 1,394
jump-table/function-boundary errors remain unhit and unaddressed.

---

## Continued: null GPU, physical aliasing, dispatcher objects

**74 of 226** imports. Three guest threads. Boot now reaches
`MmSetAddressProtect`, past GPU init and the ring buffer.

### The notable bug: physical memory aliasing

**Symptom:** SIGSEGV storing to guest `0x32003C` from
`VdEnableRingBufferRPtrWriteBack`, an address nothing had allocated.

**Root cause**, from the guest code that computed it:

```
r11 = *(r31+10768) + 60     ; 0xA0320000 + 60
r10 = r11 & 0x1FFFFFFF      ; clrlwi r10,r11,3 -- strips the top 3 bits
```

That mask is the console's virtual→physical conversion. The title took a
pointer we had handed it from the physical heap at `0xA0320000` and converted
it to physical `0x0032003C`, expecting the same bytes.

On real hardware the 512 MiB of RAM is visible through several virtual windows
that differ only in caching and page size — `0x00000000`, `0xA0000000`,
`0xC0000000`, `0xE0000000` — and guest code moves between them by masking. The
runtime was treating them as unrelated regions.

**Fix:** physical RAM is now one `memfd`, `MAP_SHARED` at all four windows, so a
write through one view is visible through the others. Pages are still allocated
lazily, so mapping four aliases of 512 MiB costs nothing until touched.
Verified: `[mem] physical RAM (512 MiB) aliased at 0x0, 0xa0000000, 0xc0000000,
0xe0000000`, and the store that faulted now lands.

This was the second bug of the same shape as the variable-import one: the guest
was right and the runtime's model was wrong.

### Guest dispatcher objects

`KeWaitForSingleObject` was failing on an address it had never seen. Titles
embed KEVENTs and KSEMAPHOREs inside their own structures and initialise them
in place, so those never pass through a handle. Host objects are now bound to
guest dispatcher objects on first use, with kind and initial state read from
the guest's own `X_DISPATCH_HEADER` rather than assumed. Semaphores were added
to the object model for this.

### NULL GPU — read this before trusting anything graphical

`runtime/vd_null_gpu.cpp` implements the 20-function `Vd*` surface **without a
GPU**. It tracks what the driver genuinely owns (ring buffer, write-back
pointer, display mode) and then retires submitted command buffers without
executing them.

Retiring is a deliberate choice, not an oversight: never advancing the read
pointer would deadlock the guest against a full ring buffer and teach us
nothing. What is missing is the command processor. Consequences, all of them
visible in the log rather than hidden:

- `VdSetGraphicsInterruptCallback` registers a callback that **will never
  fire**. A title that waits on a vsync interrupt will stall, and that stall is
  the honest signal.
- `VdSwap` counts frames and presents nothing.
- The Xenos register file at `0x7FC80000` is committed as **plain memory**.
  Guest code reaches it through the MMIO macros, not through any `Vd*` call. On
  hardware some of those registers have side effects; with no command processor
  there is nothing to trigger, so reads simply see back what was written.

### Config settings fail loudly

`ExGetXConfigSetting` answers only settings with a defensible value and returns
`STATUS_INVALID_PARAMETER` for anything else, naming the category and setting.
A wrong console setting produces misbehaviour a long way from its cause, so
guessing one is worse than refusing.

### Still true

Nothing is rendered. No file I/O, audio or input. The 1,394
jump-table/function-boundary errors remain unhit and unaddressed.

---

## Continued: real file I/O, and the first crash that is not a missing import

**102 of 226** imports.

### File I/O is real, not a null layer

`guest_filesystem` maps the console's device paths (`\Device\Cdrom0\`,
`game:\`, …) onto a host directory of extracted disc files, with a
case-insensitive fallback — the console's file systems are case-insensitive and
a Linux host is not, so a straight path join reports files missing that are
really there.

`tools/gdf_extract.py` grew `--extract-all`, resumable so an interrupted run
continues rather than restarting. The full 6.7 GB of disc data is now extracted
to `scratch/game` (6.3 GB on disk).

Unhandled `NtQueryInformationFile` classes are refused **by name** rather than
answered with zeros, because a zeroed reply looks valid. Opening a directory is
refused outright: `NtQueryDirectoryFile` is not implemented, and a handle that
silently fails every later operation is worse than no handle.

### Device MMIO window

Guest code reaches device registers at fixed addresses using byte-reversed
loads (`lwbrx` — device registers are little-endian), not through any `Vd*`
call. After the Xenos register file at `0x7FC80000`, a second block at
`0x7FEA1800` faulted. Rather than commit pages one at a time as each faults,
the whole `0x7FC00000` window is committed as one inert region. Committing a
window is a model; committing addresses as they fault is chasing symptoms.

### CURRENT BLOCKER — a real bug, not a missing import

Boot now dies with SIGSEGV in `sub_82766F68`:

```
	// lwz r31,0(r28)
	ctx.r31.u64 = PPC_LOAD_U32(ctx.r28.u32 + 0);
	// cmplwi cr6,r31,0        <- the game null-checks it, and it passes
	// lwz r30,12(r31)         <- faults here
```

`r31 = 0x3F800000`, which is the bit pattern of `1.0f` being used as a pointer.
The game's own null check passes because the value is non-zero. So the struct
`r4` points at has a float where a pointer belongs — the field was never
initialised, or something wrote a float over it.

Call path: `sub_826C49C8` → `sub_8272B7F8` → `sub_82722F38` → `sub_82766F68`.

**Ruled out:**

- *Not* caused by missing game data. The crash is byte-identical with and
  without `scratch/game` present, at the same instruction, and only one
  filesystem call happens before it.
- *Not* one of the 1,394 jump-table/function-boundary errors. None of the four
  functions in the call path appears anywhere in the recompiler's
  switch-escape error list. That hypothesis is dead; do not re-check it.

The one filesystem request seen before the crash is
`NtQueryFullAttributesFile("ShaderDumpxe:\CompareBackEnds")`, which does not
resolve. The odd-looking path is not necessarily wrong — it may be a UE3 debug
path — but it is worth confirming the `X_ANSI_STRING` length is being read
correctly before trusting it.

**Next:** trace back through the call path to find which of the four frames
produces the bad struct, and whether the float lands there from guest code or
from a wrong value one of the runtime's own stubs returned. Do not guess a fix
before that is known.

---

## Root-cause trace of the float-as-pointer crash (NOT yet fixed)

Traced the crash from symptom to the exact instruction that plants the bad
value. It is **not fixed**, but the chain is now known and four hypotheses are
dead. Read this before re-investigating.

### The chain, verified by watchpoint

The crash is a linked-list walk in `sub_82766F68`. The loop re-enters at
`loc_82766FB4`, *after* the load from `*r28`, so `r31` advances via
`r31 = *(r31+4)` (line 31465) — the node's `next` field. An early watchpoint on
`*r28` never fired, which is what proved the walk, not the head, was the
problem.

Walking back, each step confirmed with a conditional watchpoint:

1. Crash: `lwz r30,12(r31)` with `r31 = 0x3F800000`. The game's own null check
   passes because the value is non-zero.
2. The predecessor node is `0xA0730354`; its `next` field at `0xA0730358`
   holds `1.0f`.
3. That field was written by `sub_826ED298` — a textbook push-front:
   `node->next = list->head; list->head = node`. So the *head* already held
   `1.0f`.
4. The head lives at `0xA073032C`. It was written by `sub_82761CA8` line 18996:
   `stfs f0,0(r9)`, inside a float-fill loop (`addic. r10,r10,-1`) writing
   `1.0f`. Destination is `r29+36`, count `r30`.
5. At the write, `r10 == 1` — the **last** element. So the array *ends* exactly
   at `0xA073032C` rather than running past it.

That last point matters: this is not a buffer overrun. The float array and the
list header genuinely occupy the same address, so two of the title's own
objects are sharing memory.

### Ruled out — do not re-check these

- **Missing game data.** The crash is byte-identical with and without the
  6.3 GB of extracted disc data, at the same instruction.
- **The 1,394 jump-table / function-boundary errors.** None of the functions in
  the call path appears anywhere in the recompiler's switch-escape error list.
- **Overlapping runtime allocations.** `0xA0730000` is a single 64 KiB block
  from one `MmAllocatePhysicalMemoryEx`; both objects live inside it, so the
  overlap is inside the title's own arena, not between two of our allocations.
- **Forced 64 KiB physical pages.** `MmAllocatePhysicalMemoryEx` was rounding
  every allocation to 64 KiB regardless of the alignment requested (the title
  asks for `0x20`). Honouring the requested alignment is more correct and has
  been kept, but it does **not** fix the crash — same instruction, same values.

### Where to look next

The title sub-allocates inside a physical block it owns, and two of its objects
end up at the same address. So something it used to lay out that arena is
wrong. Candidates, in order:

1. A size or capacity the title derived from a value one of our stubs returned.
   `MmQueryAllocationSize` is deliberately unimplemented and would be an
   obvious one to check for — it currently traps, so it is not being called,
   but a related query might be answered wrongly.
2. The `1.0f` fill and the list are two different views of one union or pooled
   allocator; the bug may be that the title's pool is being *reset* or reused
   earlier than it should, which would point back at an event or refcount the
   runtime is signalling too early.
3. `ObDereferenceObject` is a no-op, so guest lifetime bugs do not reproduce
   faithfully. If the title relies on a refcount reaching zero to know a block
   is free, that is a real divergence and a plausible cause of two objects
   sharing memory.

Item 3 is the most suspicious, because it is a known, documented deviation
where our behaviour differs from the console rather than merely being absent.

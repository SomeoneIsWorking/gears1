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
5. ~~At the write, `r10 == 1` — the **last** element, so the array *ends*
   exactly at `0xA073032C` rather than running past it.~~
   **RETRACTED — this was wrong, see the correction below.**

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


---

## CORRECTION to the trace above — step 5 was unsound

The claim that the float array "ends exactly at `0xA073032C` rather than
running past it", and the conclusion drawn from it that this is not a buffer
overrun, are **both withdrawn**. The measurement behind them was invalid.

**Why it was wrong:** a hardware watchpoint reports *after* the storing
instruction retires. By the time gdb stops, the recompiled code has moved on
and reused the registers, so `r9`/`r10`/`r30` read at that stop describe a
later point in execution, not the state at the write. Two runs of the identical
script disagreed — `r9=0xA073032C, r10=1` once and `r9=0x1, r10=4` the next —
which is what exposed it. Register values captured at a watchpoint stop in this
codebase are not evidence.

The same objection applies to the `r30 = 2691629832` "pointer used as a count"
reading, which came from the same kind of stop and from a breakpoint condition
that matched only through unsigned wraparound. It is not evidence of anything.

**What is still solid**, because each came from the address watched rather than
from registers:

- The crash is a list walk advancing through `next` at `+4`, not re-reading the
  head. (Proved by the watchpoint on the head never firing.)
- `0xA0730358` — the predecessor's `next` — receives `1.0f`.
- `0xA073032C` — the list head — receives `1.0f` earlier.
- Both addresses lie inside one 64 KiB physical block from a single
  `MmAllocatePhysicalMemoryEx`.

**What is now unknown again:** which instruction actually writes `1.0f` to
`0xA073032C`. The backtrace pointed at `sub_82761CA8` around line 18994-18996,
but a conditional breakpoint on the store itself
(`ppc_recomp.114.cpp:18994 if ctx.r9.u32 == 0xA073032C`) **never fires**, on
either candidate line, before the process crashes. So either the write comes
from somewhere else, or it happens on another thread whose frame was not the
one inspected. The backtrace was, again, taken at a post-write stop.

**How to measure this properly next time:** do not read registers at a
watchpoint stop. Either
(a) put the conditional breakpoint on the store instruction and let it stop
    *before* the write, or
(b) have the watchpoint handler print only the *memory* it watched plus
    `$pc`, and resolve `$pc` to a source line afterwards.
Given (a) is not firing, (b) with `$pc` logging across all threads is the next
step, followed by identifying which guest allocation owns `0xA0730000` and what
the title thinks lives there.

Two lessons worth keeping beyond this bug: a hardware watchpoint stop tells you
*what changed*, never *who changed it*; and a breakpoint condition written with
unsigned guest arithmetic can match through wraparound, so conditions need to
be bounded, not just equal.

---

## Crash trace, re-done with a sound method

The retracted trace has been redone properly. The conclusion happens to land
near where the unsound one did, but it is now measured rather than inferred,
and the numbers are different in the part that matters.

### Two measurement traps, both real

**1. Hardware watchpoints do not see writes through a different virtual
alias.** Physical RAM is mapped at four guest windows (`0x0`, `0xA0000000`,
`0xC0000000`, `0xE0000000`) over one `memfd`. A watchpoint is set on a *virtual*
address, so a write through any other alias to the same physical page never
fires it. Watch all four, or watch none. This is a direct consequence of the
aliasing fix made earlier in this project.

**2. Addresses move when the allocator changes.** Making
`MmAllocatePhysicalMemoryEx` honour the requested alignment shifted every
physical allocation down by `0x40000`. Watch addresses derived before that
change silently matched nothing afterwards — the watchpoint simply never fired
and looked like "no write happens", which is indistinguishable from a wrong
address. Re-derive addresses after any allocator change.

Both traps produce the same symptom: a watchpoint that never fires. Neither is
evidence that nothing was written.

### The chain, measured

Addresses are for the current build.

1. Crash in `sub_82766F68`: `lwz r30,12(r31)` with `r31 = 0x3F800000`.
2. Predecessor node `0xA06F0354`; its `next` at `0xA06F0358` holds `1.0f`.
   Found with a *breakpoint* on the walk (registers valid), not a watchpoint.
3. Watching `0xA06F0358` on all four aliases: **exactly one write**, from the
   push-front in `sub_826ED298` (`node->next = list->head`). So the head
   already held `1.0f`.
4. Watching the head `0xA06F032C` on all four aliases: **exactly one write**,
   from `sub_82761CA8` line 18994, `stfs f0,0(r9)`.
5. Breakpoint on that store, stopping *before* it, with valid registers:

```
r9 = 0xA06F032C   remaining = 1   count(r30) = 1
fill_base = 0xA06F032C   fill_end = 0xA06F032C
```

**The fill count is 1.** It writes a single float, exactly in bounds, to
exactly that address. It is not a runaway loop and not an overrun — the earlier
"pointer used as a count" reading was an artefact and stays withdrawn.

### What this actually means

`r29 = 0xA06F0308`, and the fill target is `r29 + 36`. The list head the walker
is handed is that same `0xA06F032C`.

So one code path stores a float at object`+36`, and another treats object`+36`
as the head of a linked list. **Two subsystems disagree about the type of the
same field.**

That is either a union/derived-layout the recompiled code is reaching through
the wrong branch of, or a wrong object pointer handed to one of the two paths.
It is no longer a memory-management question — both accesses are in bounds and
the allocator is behaving.

### Next

Identify what object begins at `0xA06F0308` and which allocation produced it,
then determine which of the two paths is holding the wrong pointer. The float
path (`sub_82761CA8`) and the list path (`sub_82766F68` via `sub_82722F38`) are
both reached from `sub_826C4C28`, which is the nearest common ancestor and the
place to start.

Do **not** re-investigate: allocator overlap, buffer overrun, missing game data,
or the jump-table errors. All four are eliminated.

---

## The disputed field: whose object is it, and a vblank hypothesis (not the cause)

### Which allocation owns it

`0xA06F0308` sits in a **4 KiB physical block at `0xA06F0000`**, from a single
`MmAllocatePhysicalMemoryEx(size=4096, alignment=4096)`. Allocation path:

```
sub_826C4F28 -> sub_826C4960 -> sub_826C4648 -> sub_826C4580
             -> sub_822193B0 -> sub_82612800 -> sub_826126C0 -> MmAllocatePhysicalMemoryEx
```

The crash path descends from the **same ancestor** by a different branch:

```
sub_826C4F28 -> sub_826C4C28 -> sub_826C49C8 -> sub_8272B7F8
             -> sub_82722F38 -> sub_82766F68   (crash)
```

So one subtree of `sub_826C4F28` allocates the block and another walks it.

### How the walker derives its pointer

At `sub_82722F38` `loc_827230D0`:

```
lwz    r11,0(r30)
rlwinm r11,r11,0,0,30   ; r11 &= ~1  -- the low bit is a TAG
addi   r31,r11,-32      ; container_of: node -> owner, member offset 32
```

This is an intrusive list with a **tagged pointer**: bit 0 carries a flag and is
masked off, then 32 is subtracted to reach the containing object. So the walker
believes it holds a node whose owner begins 32 bytes earlier.

Numbers for the current build: the walker's argument is `0xA06F032C`, so the
untagged node pointer was `0xA06F034C`. The float path's object base is
`0xA06F0308` and it writes at `+36` — the same `0xA06F032C`.

`0xA06F034C - 0xA06F0308 = 0x44` (68). The two paths therefore place their
structures at different offsets within one 4 KiB block.

The tag bit is worth noting: if the low bit of that stored pointer were ever set
when the title did not intend it, the masking would still produce a clean
pointer, so a corrupted tag cannot be detected by inspecting the result.

### Vblank hypothesis — tested, NOT the cause

Reasoning: `VdSetGraphicsInterruptCallback` registered a callback that never
fired. Titles advance real state machines from the vblank interrupt, so a frozen
state machine could plausibly leave one subsystem reusing memory another still
points at — which is exactly the observed symptom.

Implemented: a host thread now drives the registered callback at 60 Hz on its
own guest thread block. `GEARS_NO_VBLANK=1` restores the never-fires behaviour
so the two can be compared.

**Result: no change.** Same crash, same function, same line, and the same
amount of progress before it (identical count of threads started and frames
submitted with the interrupt on and off). The hypothesis is **rejected** as the
cause of this crash.

The vblank thread has been kept regardless, because never raising the interrupt
is its own inaccuracy rather than a neutral omission — but it is not a fix and
must not be recorded as one.

### Where this leaves it

Both accesses are in bounds, the allocator is behaving, the interrupt is now
firing, and the two paths still disagree about the layout of one 4 KiB block.
The remaining candidates are, in order:

1. A wrong value stored into the tagged pointer at `*(r30+0)` earlier — trace
   who writes `r30`'s list rather than the object.
2. `sub_826C4F28` running its two subtrees in an order the console would not,
   e.g. because something we return causes an initialisation branch to be taken
   or skipped.
3. An actual recompiler defect in one of the two paths. Nothing points here yet
   and it should stay last, but `rlwinm rD,rS,0,0,30` and the `container_of`
   arithmetic are worth hand-checking against the PPC semantics before trusting
   them.

---

## The walker is a tagged-pointer tree traversal; recompiler semantics checked

### Candidate 3 (recompiler defect) — the two suspect instructions are correct

Hand-checked against PPC semantics rather than assumed:

- `rlwinm rA,rS,0,0,30`: rotate by 0, mask bits 0..30 in MSB-first numbering =
  `0xFFFFFFFE`, i.e. clear the LSB. Emitted as
  `__builtin_rotateleft64(...,0) & 0xFFFFFFFE`. **Correct.**
- `addi r31,r11,-32` emitted as `r31.s64 = r11.s64 + -32`. **Correct.**

That does not clear the recompiler generally, but it removes the two
instructions this bug actually depends on. Candidate 3 stays last.

### What the traversal really is

The path that produces the bad pointer is at `ppc_recomp.111.cpp:50611-50618`,
not the one at `50649` that was checked first. Confirmed by conditional
breakpoints on all three candidate sites — only the middle one fires.

```
rlwinm r11,r31,0,0,30   ; untag the current node
lwz    r11,28(r11)      ; follow the link at offset 28
clrlwi. r10,r11,31      ; test the tag bit of what was loaded
beq    loc_8272309C
  mr   r31,r20          ; tag set -> r20, which is 0: end of traversal
loc_8272309C:
  rlwinm r11,r11,0,0,30 ; tag clear -> untag
  addi   r31,r11,-32    ; container_of, member at offset 32
```

So this is a **tagged-pointer tree or intrusive list**: links live at offset 28,
bit 0 of a link is a flag, `r20 = 0` is the null sentinel, and a node is
converted to its container by subtracting 32.

Measured at the failing step: the link loaded from `+28` is `0xA06F034C` with
the tag clear, giving container `0xA06F032C`.

### The overlap, stated precisely

- Walker: container base `0xA06F032C`, node member at `+32` (`0xA06F034C`).
- Float path: object base `0xA06F0308`, writes its field at `+36`
  (`0xA06F032C`).

The two bases differ by `0x24` (36) and the objects overlap. Both live in the
same 4 KiB physical block, which — given it holds many small linked nodes — is
almost certainly a **pool the title sub-allocates from**.

A node freed back to that pool and reused while something still linked to it
would produce exactly this: one subsystem writing a float where another still
expects a live tree node.

### Why the call-site state cannot be read directly

`r30` at the call site is `0xA06F005C` and its slot holds `0xA06F0640`, which is
*not* the value that produced `r31`. The caller loops, so by the time the call
executes `r30` has already advanced. Reading iteration state at a call site in
this function is meaningless; break on the specific assignment instead, with a
condition on the value being produced.

### Next

The question is now "who returned this node to the pool, and why", not "who
wrote the float". Concretely: find the pool's free path and watch the node at
`0xA06F034C` being unlinked or freed, then work out what triggered it. That is
a guest-side lifetime question, and the runtime's most plausible contribution
remains `ObDereferenceObject` never decrementing a refcount — still untested.

---

## ObDereferenceObject rejected; and the ordering that reframes the bug

### ObDereferenceObject is NOT the cause — rejected on both evidence and reasoning

This was flagged as the prime suspect for three iterations. It is wrong, and the
reasoning error is worth recording.

**Evidence:** `ObReferenceObjectByHandle` and `ObDereferenceObject` are each
called exactly **3 times** before the crash. Perfectly balanced.

**Reasoning:** the direction was backwards the whole time. A no-op
`ObDereferenceObject` means a refcount never *drops*, so objects live **longer**
than they should. That can leak, but it cannot cause a premature free. The
symptom here needs memory released or reused **too early** — the opposite. The
hypothesis could have been discarded on that argument alone, without measuring.

Lesson: check the *direction* of a suspected divergence against the direction
the symptom requires, before spending iterations on it.

### The ordering test, and what it means

Watching the float write and the tree traversal in one run:

```
EVENT float-write-to-0xA06F032C
EVENT float-write-to-0xA06F032C
EVENT tree-link-followed-to-0xA06F032C
```

(Two float writes, not one — the earlier count of one came from a watchpoint
condition that only matched the `1.0f` value.)

**The float writes come first.** The traversal reaches that address afterwards.

This inverts the framing. It is not "two subsystems disagree about a live
field". The memory was in use as float storage, and the tree **later followed a
link into it**. The tree holds a link to a node that no longer exists — a stale
link into repurposed memory.

So the pool handed `0xA06F0308` out for float storage while the tree still
linked to `0xA06F034C` inside that same region.

### Two remaining explanations

1. **A node was freed without being unlinked**, and the pool reused it. This is
   the classic shape and fits everything observed.
2. **The tree link was never valid** — garbage from uninitialised memory that
   happens to point into the block. Less likely, because `0xA06F034C` is
   in-block and correctly aligned for a node, which garbage usually is not. But
   it becomes *more* likely if an earlier initialisation step silently failed,
   and at least one has: `NtQueryFullAttributesFile` for
   `ShaderDumpxe:\CompareBackEnds` does not resolve.

Distinguishing them: watch `0xA06F034C` from process start and see whether it is
ever written with a plausible node header before the tree links to it. If it is
never initialised as a node, explanation 2 wins and the real bug is upstream in
whatever failed silently.

### Rejected hypotheses so far — do not revisit

Missing game data · jump-table/function-boundary errors · overlapping runtime
allocations · forced 64 KiB physical pages · the vblank interrupt never firing ·
`ObDereferenceObject` refcounting · the `rlwinm`/`addi` recompiler semantics
this bug depends on.

---

## Explanation 2 rejected: the node is genuinely initialised

Watched `0xA06F034C` on all four physical aliases from process start.

```
NODEWRITE pc=0xdb3843 val(BE)=0xA06F0041     <- in-block, tag bit SET
NODEWRITE pc=0xd94b1b val(BE)=0xA06F0390     <- in-block, tag clear
TREE-FOLLOWS-NODE
```

Both writes store plausible in-block, correctly-aligned links, one of them
carrying the tag bit the traversal tests for. This is not uninitialised garbage:
the address is a real, properly constructed tree node before the tree reaches
it.

**Explanation 2 (link never valid / garbage from a silently failed init) is
rejected.** The failing `ShaderDumpxe:\CompareBackEnds` lookup is unrelated to
this crash.

Resolved writers:

- `sub_82734E60` (`ppc_recomp.112.cpp:15054`)
- `sub_82730540` (`ppc_recomp.112.cpp:4156`)

`sub_82734E60` already appeared in this investigation as the caller of the
push-front in `sub_826ED298`, so it is the structure's builder.

### What is left

Explanation 1 stands alone: **the region was handed out twice by the title's own
pool.**

Precisely: the tree's container base is `0xA06F032C`, while the float path's
object base is `0xA06F0308` and it writes at `+36` — the same address. The two
sub-allocations overlap, both inside the single 4 KiB block from one
`MmAllocatePhysicalMemoryEx(size=4096, alignment=4096)`, which we satisfy
exactly.

So the title's own sub-allocator produced two overlapping regions in a block we
sized correctly. Something it used to lay that pool out is wrong, and the only
things it could have got from us are the block's address, its size, and its
alignment — all of which match the request.

The next concrete step is to find where the pool's bump/free cursor for this
block lives, and watch it: if the cursor is reset or rewound between the two
sub-allocations, that identifies the mechanism. Until then, no fix should be
attempted — every remaining guess would be a change made without knowing why.

---

## Correction: "the pool double-allocates" was over-stated

Watching `0xA06F0308` — the address previously called "the float path's object
base" — on all four aliases from process start gives **no writes at all**.

An allocation base that is never written is not an allocation base. `r29` is a
pointer some code computes `+36` from; it does not follow that an object begins
there. Last entry's conclusion, that the title's pool handed out two overlapping
regions with bases `0xA06F0308` and `0xA06F032C`, rested on that assumption and
is **withdrawn**.

### What is actually established

The disputed word is a single address, `0xA06F032C`, and both paths agree on it:

- `sub_82761CA8` writes a **float** there (`stfs f0,0(r9)`, count 1, in bounds).
- `sub_826ED298` uses it as a **list head** (`node->next = *head; *head = node`),
  which is how `1.0f` got copied into `0xA06F0358` and became the crash's
  `r31 = 0x3F800000`.

So one word is used as a float by one subsystem and as a list-head pointer by
another. That much is measured. Whether that is two overlapping allocations, a
union, or one path holding a wrong pointer is **not** established, and the
evidence for the overlap reading has just been removed.

### Method note

This is the second conclusion in this investigation built on a register value
rather than on an address that was actually observed (`r10 == 1` was the first).
Registers describe what code *computed*; only watched addresses describe what
memory *is*. Where the two disagree, the addresses win.

Rule going forward for this bug: a claim about an object's identity or extent
needs a write to that object, not an arithmetic relationship to a register.

### Next

Determine which of the two uses is wrong by finding which one owns the memory:
watch the whole 4 KiB block's first use and identify the structure written
there, or find the allocation that returned `0xA06F032C` (as opposed to the
4 KiB block, whose origin is known). Do not assume either path is correct.

---

## Full write history of the disputed word — and which use is the intruder

Watched `0xA06F032C` on all four aliases from process start. **Exactly two
writes in the entire run:**

| # | PC | Function (post-write PC, so the store is the line before) | Value stored |
|---|----|----|----|
| 1 | `0xef2350` | `sub_82761CA8` — the `stfs f0,0(r9)` float store | `0x3F800000` (`1.0f`) |
| 2 | `0xb2149a` | `sub_826ED298` — the push-front's `stw r4,0(r31)` | `0xA06F0354` (the node) |

### Correcting the previous ordering entry

The earlier run reported "two float writes then the tree follows". That was
wrong: the printf labelled every hit `float-write` unconditionally. Write 2 is
the **list-head store**, not a float. Same trap as before — the label was an
assumption, not a measurement. Print the value, never a description of it.

### What this settles

Before write 1 the word is **zero**, and there is no earlier write. Zero is a
valid empty list head. So the sequence is:

1. The word is a zero-initialised, empty list head.
2. `sub_82761CA8` stores `1.0f` over it.
3. `sub_826ED298` pushes a node: reads the head (now `1.0f`), stores it as
   `node->next` at `0xA06F0358`, then writes the real head. That read-and-copy
   is what carries `1.0f` into the list.
4. The walker follows `next` and dereferences `1.0f`. Crash.

**The float store is the intruder.** It writes into a word that is a list head,
and the list code is behaving correctly throughout — it faithfully propagates
what it was given. This is the first time in this investigation that one of the
two uses can be called wrong rather than merely conflicting, and it rests only
on observed writes.

### Next, and narrowly scoped

In `sub_82761CA8` the destination is `r9 = r29 + 36` with a count of 1. Since
the count and the store are in bounds and correct, **`r29` is the wrong value** —
it is 36 bytes below a list head rather than pointing at whatever float field
the code intends.

So: trace where `r29` comes from in `sub_82761CA8`. That is a single-function
question with a concrete target, and it is the first step in this investigation
that starts from a known-wrong value rather than from a conflict of
interpretations.

---

## Tracing the bad pointer upward

Working up from the known-wrong value rather than from a conflict.

### The chain so far

```
sub_82761CA8   r29 = r3                 (line 18912 -- the ONLY r29 assignment)
               stores 1.0f at r29+36 = 0xA06F032C, count 1, in bounds
      ^ called by
sub_82762A08   call site line 21006 sets r4 and r5 but NOT r3
               r3 = r4  (line 20860 -- the ONLY r3 assignment before the call)
               so the bad pointer is sub_82762A08's own second argument
      ^ called by
sub_8273B488   (ppc_recomp.113.cpp:8085)
      ^ sub_82744148 -> sub_826C4C28 -> ...
```

So `0xA06F0308` enters as `sub_82762A08`'s **second argument** and is passed
straight through as `sub_82761CA8`'s first.

### Volatile-register concern — checked and cleared

`r3` is assigned at line 20860 and used at 21006, 146 lines apart. On PowerPC
`r3`-`r12` are volatile, so a call in between would clobber it and the value
reaching the callee would be stale — a plausible way for a wrong pointer to
appear, and worth ruling out explicitly.

Checked: **there are no calls between those two lines.** The value survives
legitimately. Not the mechanism.

### Next

Find what `sub_8273B488` passes as `r4` to `sub_82762A08`, and keep walking up.
The target remains a single concrete value (`0xA06F0308`) rather than an
interpretation, so each step is a mechanical question with a yes/no answer.

Worth noting for scope: this chain has already climbed four frames and is still
inside the title's own logic, with every runtime interaction so far behaving as
requested. If it reaches the top without finding a wrong value handed in from
our side, the conclusion will be that the title is being driven into a state the
console would not have produced — most likely by something we report
differently — and the search should switch from pointer provenance to which
reported value steered it there.

---

## Trace status, and why the per-frame climb is the wrong method now

Two more frames, same shape each time:

```
sub_8273B488   r31 = r3   (line 7070 -- the only assignment)  -> passes r4 = r31
sub_82744148   call site at 31756 sets r4 and r6, not r3
```

Every frame so far has exactly **one** assignment of the register in question,
and it is always "= the incoming argument". The pointer is threaded unchanged
through at least five frames. Climbing one frame per iteration is therefore
costing an iteration to learn nothing but "still passed through".

### The value's real shape

The disputed addresses are simply offsets into the 4 KiB block:

```
block base   = 0xA06F0000   (one MmAllocatePhysicalMemoryEx, size 4096, align 4096)
float object = 0xA06F0308   = block + 0x308
list head    = 0xA06F032C   = block + 0x32C   = float object + 36
```

So the question is not "which frame passes the pointer" — they all do — but
**who computed `block + 0x308`, and who decided a list head belongs at
`block + 0x32C`**. Those are two sub-allocation or layout decisions inside the
title, and one of them is wrong relative to the other.

### Better method for next time

Stop walking frames. Instead break on the *creation* of the value: find where
`0xA06F0308` is first computed, by breaking early and watching for the block
base to have `0x308` added to it, or by breaking at the allocation of the block
and single-stepping the sub-allocator that carves it up. That identifies the
layout decision directly rather than re-confirming pass-through at each level.

The alternative framing from the previous entry still stands and is now more
attractive: every runtime interaction with this block has been exactly as
requested, so if the layout decision itself is internally consistent, the fault
is upstream in some value we report that steered the title into this
configuration.

---

## Two negatives: the free policy is not implicated, and there is no pool header

### `GuestHeap::Free` never reusing memory — checked, not implicated

This is a deliberate runtime deviation (documented earlier: freed pages stay
committed and are never handed out again), so it deserved testing rather than
assuming.

Measured over a full run to the crash: **74 allocations, 10 frees.** All ten
frees are in the **title heap** (`0x40xxxxxx`). The physical block `0xA06F0000`
is **never freed**. The non-reuse policy therefore cannot have produced the
reuse seen here. It remains a leak, and should be revisited on its own merits,
but it is not this bug.

### The pointer is computed, not stored

Scanned the entire 4 KiB block at the moment of the bad store, looking for the
big-endian value `0xA06F0308` anywhere in it. **Not present.**

So the pointer is not read back out of any header, free list or descriptor
inside the block — it is computed and carried in registers and on the stack.

That argues against the "pool carves up the block and one carve is wrong"
model: a pool would keep its bookkeeping somewhere, and there is none in the
block pointing at `+0x308`.

### Where this leaves the investigation

Established, all from observed writes:

- The word at `block+0x32C` is a zero-initialised list head.
- `sub_82761CA8` stores `1.0f` over it via `r29+36`, `r29 = block+0x308`.
- The list code then faithfully propagates that value to the crash.
- `r29` is threaded unchanged through at least five frames.
- Nothing in the block records `block+0x308`, and the block is never freed.

Ruled out, cumulative: missing game data · jump-table errors · overlapping
*runtime* allocations · forced 64 KiB pages · vblank never firing ·
`ObDereferenceObject` · the `rlwinm`/`addi` semantics involved · uninitialised
tree links · volatile-register clobbering of `r3` · `GuestHeap` free-reuse
policy · a pool header inside the block.

**Assessment.** Eleven mechanisms eliminated, every one of them either in the
runtime or in the immediate data path, and none was the cause. The value is
computed inside the title from a block we allocated exactly as asked, threaded
through five frames untouched, and lands 36 bytes below a live list head.

This is the branch point flagged two entries ago, and it has now been reached.
Further pointer-provenance work is unlikely to pay. The next iteration should
**stop tracing and audit what the runtime reports about the machine** — the
values that steer the title's own layout decisions: `XGetVideoMode` and
`VdQueryVideoMode` dimensions, `VdQueryVideoFlags`, `XGetAVPack`,
`ExGetXConfigSetting` answers, `VdGetCurrentDisplayInformation` fields, and the
EDRAM/`MmQuery*` responses. A wrong answer there would make the title size or
lay out a structure differently from the console, which is exactly the shape of
what is being observed.

---

## The reported-values audit: first test is negative, and a better method exists

### Which invented values the title actually consults

All of them, before the crash:

| Import | Calls |
|---|---|
| `XGetVideoMode` | 3 |
| `VdQueryVideoMode` | 2 |
| `VdQueryVideoFlags` | 2 |
| `VdGetCurrentDisplayInformation` | 1 |
| `VdGetCurrentDisplayGamma` | 1 |
| `ExGetXConfigSetting` | 1 |
| `XGetAVPack` | 0 |

`VdGetCurrentDisplayInformation` deserves particular suspicion: its struct
layout was **invented**. Seven offsets were filled with guessed values and the
rest left zero, under a comment claiming "only the fields the title reads are
filled" — which was never verified and is not knowledge, it is a guess wearing
the costume of one. That comment has been a small lie in the source since it was
written.

### Test: does the reported resolution steer the layout?

Changed the reported mode from 1280x720 to 640x480 and re-ran.

**Result: byte-identical.** Same crash, `r31 = 0x3F800000`, `r28 = 0xA06F032C` —
the same addresses, not merely the same failure. The structure being corrupted
is laid out independently of the display configuration we report.

That is a strong negative for the whole video branch of the audit. Restored to
1280x720.

### Honest assessment after ~8 iterations on this one crash

Twelve mechanisms eliminated. The propagation chain is fully understood and
every step of it is measured. But the *cause* is not found, and the last two
promising directions — pointer provenance, and reported machine values — have
both now produced negatives.

Continuing to guess-and-eliminate from inside this runtime has poor expected
value. The remaining space is large and each probe costs an iteration.

### The method that should be used instead: differential comparison

This project has a `recomp-harness` skill describing exactly the right approach
for this situation, and it has not been used: **run a reference emulator as an
oracle and compare state until the first divergence.**

Xenia runs Gears of War. Rather than reasoning about what `0xA06F0308` should
have been, the question becomes empirical: at the same point in execution, what
does Xenia have there? The first divergence between the two is the bug, and
finding it is a search over execution time rather than over hypotheses.

That is a substantial piece of infrastructure — vendoring Xenia, driving both
headless, agreeing on comparison points — but it is the difference between
guessing and measuring, and this investigation has reached the point where that
trade is clearly worth making. It is also what the project's own methodology
says to do, and skipping it is why the last several iterations have been
hypothesis-driven rather than evidence-driven.

**Recommendation: stop single-crash archaeology and build the differential
harness.**

---

## Differential-harness feasibility: Xenia as an oracle

Assessed before committing effort, rather than starting a large build blind.

### In favour: Xenia already has the exact mechanism

`docs/instruction_tracing.md` documents built-in PPC instruction tracing:

- `#define ITRACE 1` in `x64_tracers.cc` — trace PPC instructions
- `#define DTRACE 1` — add HIR data values
- `--store_all_context_values` — full context at each step
- `#define TARGET_THREAD n` — restrict to one thread
- `bool trace_enabled` — gate tracing to a chosen point

That is precisely what a differential harness needs: a per-instruction trace
with register state, restrictable to one thread. It removes the largest piece of
work — we would not have to instrument the oracle ourselves.

### Against: Linux support is explicitly unfinished

From `docs/building.md`, verbatim:

> ### Linux
> Linux support is extremely experimental and presently incomplete.

It also expects Clang 19 specifically ("GCC, while it should work in theory, is
not easily interchangeable"); this machine has Clang 22, which may or may not
matter.

### Honest risk assessment

The crash under investigation happens early — during UE3 initialisation, before
any real rendering — so an incomplete emulator may well reach it. But
"experimental and incomplete" is the upstream project's own description, and
the failure mode is bad: effort spent getting Xenia to build and run on Linux
produces nothing at all if it cannot reach the comparison point.

This is a genuine fork in the road, not a formality:

- **Build the harness.** High cost, real risk of returning nothing, but if it
  works it turns every future divergence — not just this crash — from a
  hypothesis search into a measurement. The project's own methodology says to
  do this, and doing it late is why the last several iterations were guesswork.
- **Park this crash and broaden.** Implement more of the remaining ~120
  imports, on the theory that other paths are also blocked and progress
  elsewhere is cheaper than this one bug. Costs nothing to try, but leaves the
  first hard bug unsolved and the next one will be met with the same weak tools.

The first is correct engineering; the second is faster. Neither should be
started without the user choosing, because the first is a multi-session
commitment and the second is an explicit decision to leave a known bug.

Nothing has been built yet. The probe clone was made in scratch and discarded.

---

## X_ANSI_STRING parsing verified correct

Raised as a suspicion several entries ago ("the odd-looking path is worth
confirming before trusting it") and left open. Now closed.

Read the object attributes and counted string straight out of guest memory at
the call:

```
OBJECT_ATTRIBUTES = 0x40102368   namePtr = 0x40102360
X_ANSI_STRING     len = 29  max = 30  buffer = 0x82001B20
```

And the image at that address contains, literally:

```
b'ShaderDumpxe:\\CompareBackEnds\x00'
```

29 bytes then a terminator — exactly the declared length. **The parse is
correct**; the odd-looking path is a real string in the title's own `.rdata`,
sitting just after `"...d and new back ends.\n"`. It is a UE3 shader-comparison
debug path, not evidence of a runtime defect.

One less thing to suspect in our own code, and it closes the last open question
about the file-I/O layer.

## Decision still outstanding

The fork recorded in the previous entry — build the differential harness versus
park this crash and broaden — has not been answered, and a loop tick is not an
answer to it.

**Default if no decision arrives:** take the reversible option and broaden,
implementing more of the remaining ~120 imports. It commits to nothing, cannot
waste a multi-session effort on an emulator that may not run, and leaves the
harness available later. The harness remains the better engineering and the
recommendation stands; it simply should not be started on the strength of an
automated prompt.

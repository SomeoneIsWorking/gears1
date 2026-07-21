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

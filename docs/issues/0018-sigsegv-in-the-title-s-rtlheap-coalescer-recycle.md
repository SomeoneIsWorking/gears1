---
id: 18
title: SIGSEGV in the title's RtlHeap coalescer: recycled address space is not zeroed on commit
status: resolved
symptom: intermittent SIGSEGV in translated guest code sub_82614380 at lwz r7,4(r10) dereferencing a misaligned guest address (0x39c70865) in the unmapped hole between the heaps, with no runtime frame on the stack, after ~21800 frames
tags: memory,heap,allocator,corruption
created: 2026-07-22
updated: 2026-07-22
---

## Root cause

`GuestMemory::Commit` (runtime/guest_memory.cpp) only `mprotect`s -- it never
zero-fills. `NtAllocateVirtualMemory` promises zeroed pages on MEM_COMMIT and the
title's RtlHeap depends on it.

While the heap was a bump allocator this was invisible: every commit was of
address space nobody had used, whose pages `mmap` had already zeroed. Address-space
reuse (entry 17) broke that. `GuestHeap::Free` deliberately leaves the pages
COMMITTED and only recycles the address range, so a recycled range comes back
carrying the previous tenant's bytes.

The title's RtlHeap then reads a stale block header at the start of a segment
extension it just committed, believes it is a live heap block, and walks it.

## Evidence (core 1937065, the run that reached frame ~21840)

Fault: `sub_82614380` at guest **0x826144A0** -- NOT 0x826143F4 as first read; r28=0
so the guest took the `beq 0x82614490` branch and the faulting `lwz r7,4(r10)` is
the r31 unlink, not the r30 one.

`sub_82614380` is the RtlHeap free-block coalescer. Header layout confirmed from raw
disassembly (`tools/ppcdis.py 0x82614380 +0x200`): +0 u16 Size (16-byte units),
+2 u16 PreviousSize, +4 u8 SegmentIndex, +5 u8 Flags (bit0 BUSY, 0x10 LAST_ENTRY),
+8 Flink, +0xC Blink. r25=0xFEEEFEEE is the free-fill pattern it memsets with.

Registers at the fault: r29(heap)=0x40110000, r30(block)=0x43010000,
r31(prev block)=0x4300d3a0, r10(Flink)=0x39c70861 -> faulting address 0x39c70865.

- Heap struct at 0x40110000 carries the NT heap signature 0xEEFFEEFF at +0x10.
- Segment table (heap+0x60+i*4) = 0x40110630, 0x431A0000, 0x437A0000, 0x417B0000,
  0x43D70000 -- an EXACT match for our five MEM_RESERVE regions
  (0x40110000+0x100000, 0x431a0000+0x100000, 0x437a0000+0x200000,
  0x417b0000+0x400000, 0x43d70000+0x800000). That match validates the decode.
- **The proof the header is stale:** the block at 0x43010000 has SegmentIndex 3,
  and segment 3 is 0x417b0000+0x400000. 0x43010000 is nowhere inside it. A block
  the guest had just written could not claim that segment; the header is left over
  from an earlier tenant of those pages.
- Its PreviousSize 0x2C6 points back to 0x4300d3a0, which holds a regular 16-byte
  application-data record array (repeating `01 00 00 00 00 00 00 00 ...`), not a
  heap block. 0x39c70861 / 0xf5af0808 are two fields of that data read as Flink/Blink.
- Walking the whole 512 KiB below 0x43010000 finds no self-consistent heap chain
  (longest chain length 1), confirming the region is not heap-structured.
- The crashing run's own heap trace (scratch/logs/heap-fixed.log, 47508 lines) shows
  0x43010000 was allocated/freed repeatedly (lines 2665, 2705, 3793, 3956, 4119) and
  finally re-committed at line 47508 -- the LAST heap event before the crash.

## Second, independent defect found in the same core

Replaying the whole 46896-event trace reproduces the core's `regions_` map exactly and
finds **three double-ownership incidents**, at log lines 47505/47507/47508:
0x43010000+0x20000 over live 0x43020000+0x10000, and the same shape at 0x43060000 and
0x43090000. Cause: the "grows the existing reservation upwards" branch in
`GuestHeap::Allocate` extended `owner->second` over a live neighbouring region without
absorbing it, leaving two entries owning the same bytes. Whichever base is freed first
puts memory the other still uses back on the free list.

This is a genuine defect but it is NOT what caused this crash -- all three fired in the
final four heap operations, after the corruption already existed. Entry 17's "zero
overlap errors" claim was true of the errors it checked and blind to this shape:
`InsertFree` only checks `free_` against `free_`, never against `regions_`.

## Fix

- `runtime/guest_memory.{h,cpp}`: added `GuestMemory::Zero`. `Commit`'s comment no longer
  claims it zeroes, because it does not.
- `runtime/guest_heap.{h,cpp}`: `ZeroClaimed(address,size)` zeroes only the part of a
  newly claimed range below `everAllocatedEnd_`, the high-water mark of address space
  ever handed out. Above that mark the pages are still mmap's zeroes, so nothing is
  memset and no page is faulted in for nothing -- the cost is paid only on genuine reuse.
  Called on exactly the free -> allocated transitions: the non-fixed path, the fixed path
  with no owner, and the free gaps of a growing reservation. NOT on a re-commit inside a
  live region, where zeroing would destroy the owner's data (committing an
  already-committed page does not clear it on NT either).
- `GuestHeap::Allocate`'s grow branch now ABSORBS the live regions it extends over
  (erasing their entries, folding their extent into the owner) and warns per absorption,
  instead of silently creating a second owner. A later free of an absorbed base reports
  "free of unknown address" -- a warning and a bounded leak, which beats corruption.

## Verification

`tests/test_runtime_logic.cpp` (`TestHeapReuse`), both new and both failing before the fix:
- write 0xA5 over a 64 KiB region, free it, reallocate the same address, assert every
  byte reads back zero;
- two adjacent 64 KiB commits then a 128 KiB commit over both: assert the region count
  DROPS by one (absorbed) rather than staying flat (overlapping), and that accounting
  balances after freeing the fused base.
All runtime logic tests pass, and the absorb warning is observed firing in the test run.

## What is NOT established

- Reproduction rate. The fault was seen ONCE, in the single crashing run of entry 17;
  a sibling run reached the 900 s timeout clean. It was not reproduced on demand here, so
  "fixed" rests on the mechanism being proven from the core, not on a before/after
  crash-rate measurement. A clean run after the fix is consistent with the fix and is not
  by itself proof, given the base rate.
- Whether anything else in the title depends on committed-page contents in a way the
  zeroing now changes. The high-water-mark scheme means first-touch behaviour is byte-for-
  byte what it was; only genuinely recycled space is affected.
- The five other gears1 cores from today have unusable stacks (frames interleaving
  recompiled code with unrelated runtime symbols) and were not attributed.

### Note (2026-07-22)
Two additions made while implementing the fix.

1. HAZARD INTRODUCED AND REMOVED IN THE SAME SESSION. The first cut of ZeroClaimed ran
   BEFORE memory_.Commit in the reservation-growth path, and relied on everAllocatedEnd_
   as a proxy for 'these pages are committed'. It is not one: first fit skips a free block
   too small for the current request, and an aligned allocation leaves a head fragment, so
   free space BELOW the high-water mark need never have been committed. Memsetting it would
   have hit PROT_NONE and faulted the runtime -- turning a guest bug into a runtime crash,
   which is the opposite of what this port wants. Every claimed sub-range is now committed
   immediately before it is zeroed, and the invariant is stated on ZeroClaimed.
   A third test allocates into exactly such a never-committed head fragment and asserts it
   is handed out without faulting and reads back zero.

2. All three new checks were confirmed to FAIL against the unfixed allocator (verified by
   stashing the runtime changes and rebuilding the test): 'recycled address space is handed
   back zeroed', 'the widening commit absorbs the neighbour instead of overlapping it', and
   'fused-region accounting balances'.

RUN 1 AFTER THE FIX (900 s, GEARS_LUCENT_DEBUG=heap): reached 26160 frames at ~30 fps and
exited on the timeout. No core, zero heap errors, zero absorb warnings. That is past the
~21840 frames at which the crash occurred. It is ONE run and the fault was intermittent, so
this is consistent with the fix and is not on its own proof of it -- the proof is the stale
segment index in the core, not the crash-rate delta.

### Note (2026-07-22)
COVERAGE LIMIT, recorded so it is not mistaken for evidence later: tools/heap_replay.cpp
always calls Allocate with requestedBase=0 (line 118), because the '[heap] allocated A+S'
trace line does not record the REQUESTED base. The replay therefore CANNOT exercise the
fixed-base paths at all -- not the re-commit, not the reservation growth, not the absorb.
Replaying the crashing run through the fixed allocator is still worth something: it
reproduces the core's live-region count exactly (944) and the accounting now balances to
the byte (title 84.91 MiB live + 427.09 free = 512.00; physical 204.47 + 307.53 = 512.00),
which the overlapping-region bug would have broken. But it is NOT coverage of the absorb
fix; only the unit test is.

Worth doing if this area is touched again: log the requested base in the allocate trace so
the replay can drive the fixed-base paths too.

### Note (2026-07-22)
RUN 2 AFTER THE FIX (900 s, GEARS_LUCENT_DEBUG=heap): exit 124 (timeout), 26220 frames at
29.65 fps, no core, zero heap errors, zero absorb warnings. Together with run 1 (26160
frames) that is 2/2 clean runs past the ~21840-frame point at which the pre-fix run crashed.

REPRODUCTION RATE, stated honestly: the fault was seen ONCE, in 1 of the 2 pre-fix 900 s
runs. Two clean post-fix runs cannot distinguish 'fixed' from 'got lucky twice' at that base
rate -- roughly a 1-in-4 chance of two clean runs even with no fix at all. The confidence
here comes from the CORE, not the run count: the faulting block's SegmentIndex named a
segment it could not possibly belong to, which is only explicable as stale bytes surviving
into a fresh commit, and that is exactly what the missing zero-fill produced.

Checked and NOT attributable to this change: one '[heap:warn] free of unknown address 0x0'
appears in every run, post-fix AND in the pre-fix crashing run alike (1 occurrence each), so
it is pre-existing noise and not the absorb path's 'absorbed base freed later' warning --
absorb count was 0 in both runs.

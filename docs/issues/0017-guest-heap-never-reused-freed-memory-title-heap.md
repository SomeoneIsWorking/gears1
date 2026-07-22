---
id: 17
title: guest heap never reused freed memory: title heap exhausted after ~4800 frames
status: resolved
symptom: after ~160 s of gameplay (frame ~4800) a burst of 'out of guest heap: wanted 0x10000 bytes, 0x0 left' errors, NtAllocateVirtualMemory failures, and the title stops presenting entirely
tags: memory,heap,allocator
created: 2026-07-22
updated: 2026-07-22
---

## Root cause

`GuestHeap::Allocate` only ever advanced `cursor_`, and `GuestHeap::Free` erased the
bookkeeping entry and returned the space to nobody. The heap was a bump allocator whose
free reclaimed nothing, so EVERY guest free permanently leaked address space.

MEASURED against the leak, from a baseline trace (`scratch/logs/heap-baseline.log`,
`GEARS_LUCENT_DEBUG=heap`): at the moment of exhaustion the title heap's genuinely-live
set was only **67.9 MiB in 927 regions**. The other ~444 MiB of the 512 MiB window was
pure leak. 4725 allocations against 3641 frees.

CORRECTION to the entry that framed this: the heap that exhausts is the **title** heap
(0x40000000..0x60000000) -- the last successful allocation before the error burst is
0x5fff0000, the final 64 KiB page of that window -- NOT the physical heap. The physical
heap's live set at that moment was 204 MiB of 512 MiB and it was never close to full.

## Fix

`runtime/guest_heap.h` / `.cpp`: replaced the bump cursor with an address-keyed free list
(`std::map<uint32_t,uint32_t> free_`), first-fit, with neighbour coalescing on insert.

- Alignment is honoured INSIDE a reused block (`RoundUp(block.first, pageSize)`), not by
  assuming a free block starts aligned -- a block formed by coalescing 4 KiB-page frees
  can start anywhere. The head fragment goes back on the free list.
- Freed pages stay COMMITTED, as before and deliberately: only the address space is
  recycled. A stale guest pointer therefore still reads/writes real memory rather than
  faulting in the runtime far from the cause.
- Fixed (requestedBase != 0) allocations: a request landing inside a live region is a
  re-commit of an existing reservation and gets NO second bookkeeping entry -- otherwise
  freeing the reservation by its base leaves a stale inner entry that later releases
  address space somebody else owns. A fixed request that would overlap a region starting
  later is reported and refused an entry rather than creating two owners for the same bytes.
- `Available()` is now a real total; `GetUsage()` exposes allocated/peak/free/largestFree/
  freeBlocks/regions, and the exhaustion error now prints free bytes, block count and
  largest USABLE block so fragmentation is distinguishable from genuine demand.
- Peak is reported through lucent at each 16 MiB step, which is what makes "does it
  plateau" answerable from a log.

`runtime/kernel_memory.cpp`: `NtFreeVirtualMemory` now honours FreeType. This is REQUIRED
by reuse and was harmless before: MEM_DECOMMIT (0x4000) drops pages but KEEPS the
reservation, so releasing the range would hand address space the guest still owns to
another allocation. Only MEM_RELEASE (0x8000) frees. FreeType 0 is treated as a release,
matching the previous unconditional behaviour so no caller silently starts leaking.

## Verification (measured, two runs)

Baseline (`gears1-baseline`, 780 s): steady 29.90 fps, error burst begins immediately
after frame **4800**, and VdSwap NEVER appears again. The log's last write is ~3 min into
a 13-min run: after exhaustion the process sat SILENT for 10 minutes and was killed by the
timeout (exit 124).

Fixed run 1 (900 s, heap trace on): reached frame **21840** at 29.89 fps.
Fixed run 2 (900 s, no heap trace): reached frame **25560** at 29.92 fps, exit 124 (timeout).

Zero `out of guest heap` and zero overlap errors in both.

PLATEAU CONFIRMED, and it is a plateau rather than slower growth: peak stops moving at
title **192 MiB** / physical **194 MiB** and never rises again -- run 1's last peak report
is at log line 4297 of 47508 (~frame 540), flat across the remaining 21300 frames; run 2
reports the IDENTICAL two figures at line 107 of 599 and stays flat. Both well under the
512 MiB window. 23996 allocs / 22900 frees in run 1.

Replaying the recorded baseline request stream through the real allocator
(`tools/heap_replay.cpp`, built as `heap_replay` with the tests) gives title peak
195 MiB / physical 208 MiB, 0 failures -- agreeing with the live runs.

Unit tests added to `tests/test_runtime_logic.cpp` (`TestHeapReuse`): reuse of a freed
region at the same address, 64 cycles of 8-block churn through a 1 MiB heap (impossible
without coalescing), free list collapsing back to ONE block, alignment honoured out of a
reused block, non-overlap of live allocations, re-commit-inside-reservation not creating a
second region, and exhaustion still reported.

## What is NOT established

- **The downstream SIGSEGV/interrupt error was NOT reproduced as a consequence of
  exhaustion.** The baseline did not print an unanticipated-interrupt error and did not
  take SIGSEGV within 780 s; it went silent and stopped presenting. So the previous claim
  that those follow from exhaustion is UNCONFIRMED here -- the observable consequence of
  exhaustion is a permanent presentation stall, not a crash.
- Fixed run 1 DID take a SIGSEGV at ~frame 21840, far past anything the baseline reached.
  It is INTERMITTENT: run 2 ran the full 900 s to frame 25560 with no crash. The fault is
  in translated guest code (`sub_82614380` <- `sub_82615040` <- `sub_82615B98`, at
  `lwz r7,4(r10)`) with NO runtime frame on the stack; the dereferenced guest address was
  `0x39c70865` -- misaligned and in the unmapped hole between the heaps. Recycled heap
  memory stays committed and therefore CANNOT fault, so this is a garbage pointer, not a
  recycled-block access. Whether reuse contributes to producing that garbage pointer is
  UNTESTED; it needs its own investigation.
- Reuse does change the failure mode of guest bugs: a use-after-free now reads memory that
  may have been handed to another owner, so a latent UAF can become silent data corruption
  instead of touching quiescent leaked memory. This is a real risk and is the most likely
  way this change could make something else worse.
- **NEW, separate defect surfaced (pre-existing, previously unreachable):** the guest asks
  for fixed allocations at base `0x12000` (type 0x60001000, sizes 0x10000/0x20000), which
  fall outside the title heap window and are refused -- `fixed allocation 0x10000+0x20000
  is outside the heap 0x40000000+0x20000000`. The baseline never survived long enough to
  reach this. Not folded into this fix.

### Note (2026-07-22)
FOLLOW-UP, and two corrections to this entry -- see #18 for the full investigation.

1. The SIGSEGV listed under 'What is NOT established' is now root-caused and it IS a
   consequence of this change, though not of the reuse logic itself: GuestMemory::Commit
   never zero-filled. That was harmless while the heap only moved forwards (fresh mmap
   pages are already zero) and became fatal the moment address space was recycled, because
   Free deliberately keeps the pages committed. The title's RtlHeap read a stale block
   header out of a segment extension it had just committed. Fixed in #18.

2. CORRECTION to 'Zero out of guest heap and zero overlap errors in both': no OVERLAP error
   fired, but overlaps were present. Replaying the crashing run's trace, and dumping the
   live regions_ map out of core 1937065, both show THREE pairs of overlapping live regions
   (0x43010000+0x20000 over 0x43020000+0x10000, and the same shape at 0x43060000 and
   0x43090000). The detector structurally could not see them: InsertFree compares free_
   against free_ only, and nothing ever compared regions_ against regions_. The producer was
   the 'grows the existing reservation upwards' branch of Allocate, which predates this
   commit. Also fixed in #18.

3. CORRECTION to the fault location quoted here: the faulting instruction is at guest
   0x826144A0, not 0x826143F4. Both are 'lwz r7,4(r10)'; r28 was 0, so the guest branched to
   0x82614490 and the fault is the r31 unlink.

---
id: 47
title: Free of unknown address 0x0 on every run - kernel frees were not routed by address and had no NULL contract
status: resolved
symptom: "[heap:warn] free of unknown address 0x0, exactly once per run, early, just after VdSetDisplayMode and XGetVideoMode"
tags: memory,heap,allocator,kernel,d3d
created: 2026-07-29
updated: 2026-07-29
---

THE MESSAGE THAT ACTUALLY APPEARS TODAY IS ONLY THE 0x0 ONE. Older logs also
carry `0x40200000`, `0x521a0000` and dozens of other 64 KiB-aligned addresses,
but those stopped when the fixed-allocation absorb path landed (the run still
warns about the absorb itself, which is a different and already-explained
thing). wf1.log, wf2.log and wf3.log each contain exactly ONE unknown free, and
its address is 0.

CAUSE, traced statically in the recompiled code rather than guessed:

  sub_822154B4 / sub_822154D8   D3D resource release, refcount reaches 0
        |  loads r5 from the object -- the flag that says physical or virtual
  sub_82214C70                  frees the buffer at *(resource+0) and then
        |                       zeroes +0/+4/+8. NO NULL CHECK on the pointer.
        +--- r5 != 0 --> sub_82612758  (XPhysicalFree)
        |                    `mr r4,r3 ; li r3,0 ; b MmFreePhysicalMemory`
        +--- r5 == 0 --> sub_82612658  (VirtualFree, freeType MEM_RELEASE,
                             size 0) -> NtFreeVirtualMemory with base 0

Neither wrapper guards NULL, so a resource that never got a buffer sends 0 into
the kernel free. That is normal traffic from the guest, not a runtime bug.

WHAT THE RUNTIME DID WRONG: `NtFreeVirtualMemory` called `TitleHeap().Free(0)`
and `MmFreePhysicalMemory` called `PhysicalHeap().Free(0)` unconditionally --
no NULL contract, and no routing by address at all. The allocator was left to
report a legal guest idiom as an error, in a message that did not even say
WHICH of the two heaps it came from.

THE FIX, at the kernel entry points, where the contract lives:

  - `gears::HeapForAddress(uint32_t)` is new and mirrors Xenia's
    `Memory::LookupHeap` (memory.cc), which dispatches on the ADDRESS alone.
    Frees have to route this way precisely because sub_82214C70 picks the
    export from a flag on the resource, not from where the memory came from.
    Address 0 belongs to no heap.
  - `NtFreeVirtualMemory`: a zero base returns STATUS_MEMORY_NOT_ALLOCATED
    before any heap is consulted, which is exactly what Xenia's
    `NtFreeVirtualMemory_entry` does; the title's VirtualFree wrapper turns that
    negative status into a FALSE return, as on the console. An address outside
    the title heap returns STATUS_INVALID_PARAMETER (Xenia refuses a heap whose
    type is not kGuestVirtual). A failed release now returns
    STATUS_UNSUCCESSFUL -- this used to return SUCCESS unconditionally, telling
    the title a free it never performed had worked.
  - `MmFreePhysicalMemory`: NULL is a documented no-op at debug level; a
    non-physical address warns WITH THE GUEST LR instead of being handed to the
    physical heap, where it could only ever report as unknown.
  - `GuestHeap::Free`'s warning now names the heap it was asked to free from.

WHAT WAS NOT DETERMINED, and where the next session should start if the warning
survives: which of the two branches of sub_82214C70 actually delivers the 0 was
never established, because the runtime does not record the caller of a free.
Both were fixed, so it does not matter for the fix -- but the new warnings do
carry the LR, so a survivor now names itself. `ExFreePool`, `XamFree` and
`RtlFreeAnsiString` still call `TitleHeap().Free` unrouted; there is no evidence
any of them is ever reached with an address the title heap does not own, so they
were deliberately left alone rather than changed on speculation.

VERIFICATION: `tests/test_runtime_logic.cpp` `TestHeapRouting` pins the two
windows to their bases and checks both edges of each, that 0 and the loaded
image belong to no heap, and that a real physical allocation routes back to the
physical heap and not the title heap. Full runtime build + all 7 ctest suites
pass. NOT verified on a real run -- the agent that wrote this could not run the
game, so the claim that the warning disappears is a prediction, not a result.

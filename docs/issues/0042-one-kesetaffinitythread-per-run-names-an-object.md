---
id: 42
title: One KeSetAffinityThread per run names an object the runtime cannot map back to a thread
status: resolved
symptom: [thread:warn] KeSetAffinityThread(object=0x42942000, mask=0x10): no thread behind that object -- exactly once per run, for the thread entered at 0x82941df0
tags: kernel,thread,affinity,audio
created: 2026-07-28
updated: 2026-07-29
---

Found immediately after making KeSetAffinityThread honour the processor number
(catalog #40). Every other affinity call in a run resolves; this one does not.

The thread is created with cpu 1 from its creation flags and then asks for
mask 0x10, i.e. cpu 4. Because the object cannot be mapped back, it keeps cpu 1.

WHY IT PROBABLY HAPPENS: KeSetAffinityThread receives the guest pointer the
title holds for the thread, which the runtime mints lazily in
GuestAddressForHandle. HandleForGuestAddress reverses that map, so a pointer
minted for a DIFFERENT handle onto the same thread -- NtDuplicateObject creates
exactly that -- resolves to a handle that is not in the thread table. Not
confirmed; the alternative is that the pointer never came from
GuestAddressForHandle at all.

WHY IT IS NOT URGENT: the audio worker, which is what per-CPU numbering was
broken for, is placed correctly by its creation flags and never calls this. The
thread that misses out is 0x82941df0, whose per-CPU state nothing has been shown
to read.

WHY IT IS NOT NOTHING: it is silent-wrong state of exactly the kind that cost
this session a day. If some table is indexed by this thread's processor number,
it will disagree, and the failure will look like anything but an affinity call.
The warning stays loud so it cannot rot quietly.

THE FIX IS STRUCTURAL, WHICH IS WHY IT IS NOT DONE HERE: the runtime has TWO
objects per thread -- the KTHREAD in the thread block it populates, and the
synthetic dispatcher header GuestAddressForHandle mints for the handle. Xenia
has one. Collapsing them so the title's pointer IS the KTHREAD would remove this
lookup entirely rather than making it cleverer, and would fix every other API
that takes a thread pointer at the same time.

### Resolution (2026-07-29)
CONFIRMED and fixed. The hypothesis in this entry was right, and scratch/logs/phys2.log lines 179-181 prove it on three consecutive lines:

    [kernel] NtDuplicateObject(0xf800003c) -> 0xf8000044
    [kernel] ObReferenceObjectByHandle(0xf8000044) -> 0x42932000
    [thread:warn] KeSetAffinityThread(object=0x42932000, mask=0x10): no thread behind that object

The chain, from the recompiled image: the title creates the thread whose entry is
0x82941df0 at 0x8294214C, then at 0x8294218C calls DuplicateHandle (guest
sub_8294F198 -> NtDuplicateObject) on the handle it got back. It is the DUPLICATE
it later passes to XSetThreadProcessor (guest sub_82613900), which does
ObReferenceObjectByHandle -> KeSetAffinityThread(object, 1 << processor, &prev).
A duplicate is a second handle onto the SAME host object, and the runtime keyed
BOTH the thread table and the guest-address map by HANDLE -- so the duplicate got
its own freshly minted guest address that mapped back to a handle no thread table
knew. Nothing was missing; the identity was wrong.

FIX: the object, not the handle, is a threads identity, and that rule now applies
everywhere. GuestAddressForHandle is keyed by the object, so every handle onto one
object yields ONE guest pointer (as on the console). kernel_thread.cpp keys its
thread records by the KernelObject. The resume gate is keyed the same way, which
also deletes its reverse scan over the handle->address map. HandleForGuestAddress
had no callers left and is gone.

Two other bugs fell out of reading the real export. Xenias disassembly note
(xboxkrnl_threading.cc:359) says the consoles KeSetAffinityThread returns an
NTSTATUS and reports the PREVIOUS affinity through the pointer argument; ours
returned the requested mask and echoed it back, which told the caller nothing.
And ProcessorNumberFromMask folded an out-of-range mask back with `% 6`, inventing
a processor -- 0x40 became processor 0. It now answers kProcessorNone.

NOT DONE, and still worth doing: the runtime still has TWO objects per thread (the
KTHREAD in the thread block, and the synthetic dispatcher header minted for the
handle). Collapsing them so the titles pointer IS the KTHREAD would additionally
make KeWaitForSingleObject on a thread pointer wait on that thread, and make the
pointer compare equal to KeGetCurrentThread(). Not done here because it changes a
guest-visible pointer and could not be verified without running the title.

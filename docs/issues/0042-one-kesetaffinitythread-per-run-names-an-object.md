---
id: 42
title: One KeSetAffinityThread per run names an object the runtime cannot map back to a thread
status: open
symptom: [thread:warn] KeSetAffinityThread(object=0x42942000, mask=0x10): no thread behind that object -- exactly once per run, for the thread entered at 0x82941df0
tags: kernel,thread,affinity,audio
created: 2026-07-28
updated: 2026-07-28
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

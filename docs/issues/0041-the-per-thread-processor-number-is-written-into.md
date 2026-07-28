---
id: 41
title: The per-thread processor number is written into KTHREAD+0x14C, which Xenia says is thread_id
status: resolved
symptom: no observable symptom yet: the value is distinct per thread, which is all the adaptive lock needs, so a wrong field reads as working
tags: kernel,thread,layout,latent
created: 2026-07-28
updated: 2026-07-28
---

Found while fixing the KPCR current_cpu field for the audio barrier (catalog
#40), which is the same class of bug done right.

guest_thread.cpp writes the round-robin processor number to
`thread + kThreadProcessorNumber`, with kThreadProcessorNumber = 0x14C, and a
comment saying sub_827A7B08 reads it to decide whether a spinning thread is on
the lock holder's core.

Xenia's X_KTHREAD (extern/xenia/src/xenia/kernel/xthread.h) puts:

    uint8_t  current_cpu;   // 0xBF
    xe::be<uint32_t> thread_id;  // 0x14C

So 0x14C is the THREAD ID, and the real current_cpu is a byte at 0xBF.

WHY IT HAS NOT BITTEN: the only requirement the adaptive lock places on the
value is that it differ between threads, and a round-robin 0..5 does differ.
A thread id would satisfy the same comparison. So the lock works, and the field
being wrong is invisible -- until something reads either field for what it
actually means.

WHAT IT WOULD COST TO GET RIGHT: write the processor number to 0xBF as a byte
(matching what KPCR+0x10C now gets) and, separately, publish the real thread id
at 0x14C. That is a small change, but it alters a field the title's lock reads
today, so it wants its own verification run rather than riding along with an
unrelated fix.

NOT DONE, DELIBERATELY: changing a field the startup lock depends on, in the
same commit as an audio investigation, is how a working boot becomes an
unexplained hang. Recorded so the next session changes it on purpose.

### Resolution (2026-07-28)
Fixed as part of the audio-worker processor fix (catalog #40), because that work
made the whole per-CPU story correct at once rather than leaving one field right
and its neighbour wrong.

KTHREAD+0xBF now carries current_cpu, written together with KPCR+0x10C whenever
a thread's processor is set. KTHREAD+0x14C now carries a real per-thread id from
a counter, which is what Xenia's layout says the field is.

The worry recorded here -- that changing a field the title's adaptive lock reads
could turn a working boot into an unexplained hang -- did not materialise, and
the reason it was safe is worth keeping: the lock only needs the value to DIFFER
between threads, and a counter differs exactly as a round-robin processor number
did. Verified on a full run to gameplay: 1860 frames at 29.96 fps, no new
warnings, both test suites passing.

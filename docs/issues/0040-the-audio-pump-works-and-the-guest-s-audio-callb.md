---
id: 40
title: The audio pump works, and the guest's audio callback needs KeWaitForMultipleObjects
status: open
symptom: with the audio pump driving the registered callback, the title aborts on the unimplemented import KeWaitForMultipleObjects (SIGABRT, exit 134)
tags: audio,xaudio,kernel,imports,blocker
created: 2026-07-28
updated: 2026-07-28
---

STEP (a) OF CATALOG #39 IS DONE AND IT ANSWERED THE NEXT QUESTION.

The pump is implemented in runtime/xaudio_null.cpp: a host thread with a
GuestThreadBlock and a PPCContext calls the registered guest callback through
PPC_LOOKUP_FUNC at 187.5 Hz (48000/256, Xenia's AudioDriver contract), passing
the context captured at registration in r3 -- the same mechanism vd_null_gpu.cpp
uses for the graphics ISR.

It works. The thread starts, reaches the callback, and the callback runs guest
code. What the guest code does immediately is call an import we do not have:

    [import:error] unimplemented: KeWaitForMultipleObjects
    [import:error]   r3=0x2 r4=0x403e2e90 r5=0x1 r6=0x3
    [import:error]   r7=0x1 r8=0x0 r9=0x0 r10=0x403e2ea0

Decoded against the NT signature KeWaitForMultipleObjects(Count, Object[],
WaitType, WaitReason, WaitMode, Alertable, Timeout, WaitBlockArray): TWO objects,
WaitType 1 (WaitAny), no timeout (r9 = 0), with the object array and wait blocks
on the pump thread's own stack -- which is itself confirmation that the callback
is executing in our context as intended.

So the audio path is: pump -> guest callback -> waits on two dispatcher objects
-> (presumably) submits a frame. The wait is the next dependency.

THE PUMP IS OFF BY DEFAULT (GEARS_AUDIO_PUMP=1 enables it). This is not
timidity: driving the callback turns a stable, silent title into one that aborts,
which is strictly worse than silence. It stays in the tree, off, so the next
session has a one-line reproduction.

WHAT KeWaitForMultipleObjects NEEDS: the single-object path already exists
(kernel_object_api.cpp) and is built on BindGuestDispatcherObject(addr) plus a
blocking object->Wait(timeout). A correct WaitAny cannot be built from those
alone -- it has to wait on several objects at once, so it wants a wait mechanism
the dispatcher objects share rather than one blocking call per object. Polling
them in turn would satisfy the caller and mis-order every wake; that is the
shortcut to avoid.

### Note (2026-07-28)
PROGRESS: the abort is gone, and the blocker has moved one step further in.

KeWaitForMultipleObjects is now implemented (kernel_object_api.cpp), and with it
the audio callback no longer aborts -- a 75 s run with GEARS_AUDIO_PUMP=1 is
healthy throughout, rendering at ~30 fps, with both test suites passing.

IT REQUIRED A REFACTOR, and the reason is worth recording because the shortcut is
tempting. Dispatcher state used to live under a mutex and condition variable PER
OBJECT. A multi-object wait cannot be built on that: a synchronisation event or a
semaphore is CONSUMED by the waiter that takes it, so testing objects one at a
time can swallow a signal from an object the caller then abandons, and a WaitAll
would consume some objects while still blocking on another. The test and the
consume have to be atomic across every object. All dispatcher state now lives
under ONE shared lock with one condition variable, which makes
KernelObject::WaitMultiple obviously correct; the cost is that a signal wakes
every waiter to re-check its own predicate, which is far cheaper than the
bookkeeping to avoid it at the rate a title signals events.

ALSO FIXED: the driver handle. Xenia returns 0x41550000 | index -- an 'AU' magic
in the top half that XAudioSubmitRenderDriverFrame asserts on -- and we returned
a bare 1. Any title that checks or unpacks the handle was seeing a value the
console would never produce.

WHERE IT STOPS NOW: the callback is entered and BLOCKS. The pump's invocation
counter never reaches 1, which it logs after the call returns, so the very first
invocation is still inside the guest -- waiting, with no timeout, on the two
objects it asked KeWaitForMultipleObjects about. Nothing signals them.

So the callback is not the simple fill-this-buffer function the driver model
suggests; in this title it waits for something else first. The next question is
WHAT signals those two objects on hardware -- most likely the title's own audio
thread, which may in turn be waiting on a subsystem we do not drive (XMA decode
is a candidate: XMACreateContext hands out contexts with no decoder behind them
and reports as much). Identifying the two objects is the concrete next step: they
are at the guest addresses in the object array, and BindGuestDispatcherObject
already names them.

THE PUMP STAYS OPT-IN. It no longer crashes, but it achieves nothing yet and
leaves a guest thread blocked forever, so there is no reason to make it the
default until it produces a frame.

### Note (2026-07-28)
THE BLOCKED WAIT IS NOT THE PROBLEM -- IT IS A SYMPTOM OF A SPINNING BARRIER.

Two instruments were built first, because the old evidence could not tell
"blocked forever" from "never reached", and could not say WHICH thread was
which:

1. KeWaitForMultipleObjects now serves an untimed wait as a loop of 5 s waits
   and warns once when the first slice expires, naming every object and its
   kind. WaitMultiple consumes nothing on timeout, so the semantics are
   unchanged. A wait nothing will satisfy now says so in a default run.
2. Every log line on the wait channel is prefixed with the guest thread running
   on that host thread (guest_thread.h: SetGuestThreadName/GuestThreadName),
   set for title threads, the audio pump and the graphics ISR.

With those, the sequence is unambiguous:

  [guest-4]    KeWait -> 0x82becc28                 (audio worker, created suspended,
                                                     entry 0x825ea130, waits to be kicked)
  [audio-pump] KeSetEvent(0x82becc28)               (our pump, inside the callback)
  [guest-4]    KeWait <- 0x82becc28 signalled
  [audio-pump] KeWaitMultiple -> WaitAny on
               [0x82becc04 sync-event] [0x82becc48 notification-event]
  [audio-pump] ... has blocked 5 s ...; nothing has signalled them

So the callback kicks the title's audio worker and waits for it to answer. The
worker is what never answers. gdb on the live process (thread apply all bt)
puts guest-4 NOT in a wait at all but executing guest code:

  #0 sub_825EAA68  #1 sub_825EA930  #2 sub_825EA130

sub_825EAA68, read from the recompiled source, is a RENDEZVOUS BARRIER:

  if (load32(r3+304) == 0) return;
  store8(r4 + load8(r13 + 268), 1);      // check in at my slot
  expected = packed bytes, one per CPU, from the dwords at r3+308..r3+328
  spin until load64(r4) == expected  (or == 0)

r13 is the KPCR, and KPCR+268 (0x10C) is prcb_data.current_cpu -- Xenia's
X_KPCR has X_KPRCB embedded at 0x100 and current_cpu at +0x0C. The runtime
never populated it, so EVERY thread read CPU 0 and checked in at slot 0.

FIXED (guest_thread.cpp): the per-thread processor number is now written to
KPCR+0x10C as well as to the KTHREAD field. This is the same class of bug as
the scheduler tick already documented in that file -- a field the console
maintains that we left zero -- and it is correct on its own merits.

IT DID NOT UNBLOCK THE AUDIO, and the reason is the real remaining problem.
Reading the participant dwords out of the live process (+308 = 0xffffffff,
+312 = 0x00000a00, +316..+328 = 0) the expected mask is 0x0101000000000000:
big-endian bytes 0 and 1, i.e. TWO participants, on CPU 0 and CPU 1.

Only ONE guest thread ever enters this barrier: the log shows exactly one
ExCreateThread with entry 0x825ea130 for the whole run. So the barrier waits
for a second participant that does not exist here, and no amount of CPU-number
correctness fixes that by itself.

NEXT, and it is a question about the title's design rather than a knob: WHAT is
the second participant on hardware? Either the title creates a second audio
thread by a path we do not reach, or the participant is the XAudio driver side
-- i.e. us -- in which case the pump thread is standing in the wrong place: it
enters the callback and waits, when the console's audio hardware thread would
also be checking into this barrier. The decisive instrument is a hardware
watchpoint on the guest dwords at r3+308/+312 to catch what writes them and
from which thread; they are already written by the time the barrier spins, so
the watchpoint has to be armed before audio init, on an address recovered from
a prior run.

DO NOT try to satisfy the barrier by writing the missing byte. It would make
the callback return and prove nothing about whether the frame it then submits
is real.

### Note (2026-07-28)
RESOLVED, AND ONE EARLIER MEASUREMENT IN THIS ENTRY WAS WRONG -- CORRECTED BELOW.

CORRECTION FIRST. The previous note claimed the barrier's participant mask was
0x0101000000000000, "two participants, on CPU 0 and CPU 1". That was read out of
gdb by printing ctx.r3/ctx.r4/ctx.r10 in an optimised frame, where the register
fields had not been written back to the PPCContext -- one of the values printed
was obvious garbage, which should have been the tell. Re-measured properly, by
following the fixed global pointer at 0x82BFAAAC to the audio context at
+0x4016033c and dumping guest memory there:

    +304 = 1                 (the gate/count)
    +308 = 0   +312 = 0   +316 = 0
    +320 = 0   +324 = 0xf8000014   +328 = 0

+324 is the FIFTH per-CPU slot and it holds guest thread 4's own handle. The mask
is therefore 0x0000000001000000 -- ONE participant, on CPU 4, and that
participant is the audio worker itself. The barrier was never waiting for a
second thread. It was waiting for the one thread it had to check in at slot 4,
while that thread checked in at slot 0.

ROOT CAUSE: the runtime invented processor numbers instead of taking the
title's. On the console the processor comes from the top byte of ExCreateThread's
creation flags as a one-hot mask, empty meaning "inherit my creator's", and
KeSetAffinityThread moves a thread afterwards; both write KPCR+0x10C
(prcb_data.current_cpu) and KTHREAD+0xBF (current_cpu). We honoured neither and
assigned round-robin in creation order, so the number every per-CPU table in the
title is indexed by was unrelated to the number the title had chosen.

Gears creates its audio worker with cpu 4 in the creation flags, stores the
handle in slot 4 of the audio context, and the worker checks in at
`array[KPCR[0x10C]]`. With the number taken from the title rather than invented,
the slot the worker writes and the slot the mask expects are the same one.

FIXED, and audio frames now flow:

  [audio-pump] KeSetEvent(0x82becc28)          kick the worker
  [guest-4]    KeWait <- 0x82becc28 signalled
  [audio-pump] KeWaitMultiple -> WaitAny on [0x82becc04] [0x82becc48]
  [guest-4]    KeSetEvent(0x82becc04)          worker answers
  [audio]      1 frames submitted

Over a 60 s run: 11250 callback invocations and 11250 frames submitted by the
title, one for one, which is exactly 60 s at the driver's 187.5 Hz. Rendering is
unaffected at 29.96 fps and both test suites pass.

The samples pointer is stable at 0x40165380 and no submission arrives without a
buffer. WHAT IS STILL NOT PROVEN is that those samples are audible sound rather
than 11250 buffers of silence -- XMA contexts are still handed out with no
decoder behind them, and nothing has looked at the PCM. That is step (b) of
catalog #39 and it now has something real to look at.

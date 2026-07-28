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

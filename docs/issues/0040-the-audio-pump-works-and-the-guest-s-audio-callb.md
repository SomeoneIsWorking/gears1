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

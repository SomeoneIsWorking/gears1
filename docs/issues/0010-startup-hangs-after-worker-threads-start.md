---
id: 10
title: startup hangs after worker threads start
status: resolved
symptom: no crash; main thread and workers block forever in WaitOn(-1) via sub_8243A510 -> sub_82613318 -> sub_82613DA8, while one worker spins in an mftb-bounded loop in sub_82221A68 -> sub_8222F460
tags: threading,startup,timebase
created: 2026-07-22
updated: 2026-07-22
---

## Root cause


## What was tried / dead ends


## Resolution

### Note (2026-07-22)
Thread stacks taken by attaching gdb to the live hung process. Threads 1 (main), 3, 4, 8 and 9 are all in KernelObject::Wait(-1) from WaitOn, each on a different event handle (0xF8000000, 0xF8000008, 0xF800001C, 0xF8000024, 0xF800002C), all reached through the same wrapper sub_8243A510 -> sub_82613318 -> sub_82613DA8. Thread 2 is the only one running guest code, spinning in sub_82221A68 (worker entry sub_8243AD98), which reads mftb and compares counters at r31+0x2A10 / r31+0x2A1C, calling sub_8222F460 each pass.

Preceding fix, already landed: KeResumeThread is passed a thread OBJECT pointer while NtResumeThread is passed a handle, and both fed ResumeThread which only knew handles, so a suspended worker never woke. That is fixed and verified -- all workers now enter guest code -- but the hang persists, so it was a separate defect and NOT the cause of this one.

Open question, deliberately not yet claimed as the cause: mftb was recompiled as raw __rdtsc(). The Xenon time base is 49.875 MHz and the host TSC is around 3 GHz, so tick-to-time conversions are off by roughly seventy times. That is a genuine defect being fixed regardless, but note the direction of the error argues AGAINST it explaining this hang: a loop bounded in ticks would expire far too EARLY, not spin forever. Do not record it as the root cause without evidence.

### Note (2026-07-22)
CORRECTED PICTURE from a clean single-boot run (an earlier log had two boots appended and its counts were not trustworthy -- do not reuse that analysis).

Waits entered: 0xF8000000 x1, 0xF8000008 x3, 0xF8000018 x1, 0xF800001C x1, 0xF8000024 x1, 0xF800002C x1 (8 total). Waits returned: 0xF8000008 x2, 0xF8000018 x1 (3 total). The 5 that never return match the 5 threads gdb shows blocked.

Signals in the whole run: NtSetEvent on 0xF8000008 x2, 0xF8000018 x1, 0xF8000030 x1. Nothing ever signals 0xF8000000, 0xF800001C, 0xF8000024 or 0xF800002C. Notably there are ZERO KeSetEvent calls, so the handle-vs-object-pointer split that caused the resume bug is NOT in play for events.

Conclusion: the blocked threads are not the defect. They are waiting for work that would be produced by thread 2, which is the only thread running guest code and is stuck in its own loop in sub_82221A68 (worker entry sub_8243AD98), reading counters at r31+0x2A10 / r31+0x2A1C and calling sub_8222F460 -> sub_827A7B08 each pass. The starving waiters are a consequence. Investigate sub_82221A68's loop condition next, NOT the event plumbing.

Ruled out: mftb rate (fixed to 49.875 MHz, hang reproduces identically); thread resume handle/pointer split (fixed, all workers now run, hang persists).

### Resolution (2026-07-22)
KTHREAD+0x58 (scheduler tick) and KTHREAD+0x14C (processor number) were never populated by the runtime. The title's adaptive lock reads +0x58 to decide whether to keep spinning ('tick - tickAtLastProgress < 5000'); with the field pinned at zero the delta was always zero, so it spun forever and never fell through to its blocking path. Four read sites in the whole image, zero writers -- purely kernel-maintained fields the runtime owns. Fixed by publishing a millisecond tick into every live thread block from one writer and assigning each thread a distinct processor number. Startup now proceeds past the lock.

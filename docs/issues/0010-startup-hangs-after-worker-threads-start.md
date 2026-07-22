---
id: 10
title: startup hangs after worker threads start
status: investigating
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

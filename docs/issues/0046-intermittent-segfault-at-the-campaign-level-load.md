---
id: 46
title: Intermittent segfault at the campaign level load, roughly one run in three
status: open
symptom: exit 139 at ~1800 frames on about one run in three; the other runs reach 5000-7000 frames
tags: crash,memory,threads,saves
created: 2026-07-29
updated: 2026-07-29
---

After the content-protocol fixes the title reaches the campaign level load. It now crashes INTERMITTENTLY rather than every run, and when it does not crash it runs several times longer than it ever has.

MEASURED, three consecutive runs, 240 s timeout, identical scripted input:

    run 1   exit 124 (survived the timeout)   6840 frames
    run 2   exit 139 (SIGSEGV, core dumped)   1860 frames
    run 3   exit 124 (survived the timeout)   5340 frames

About one run in three, at roughly the same point (~1800 frames, the level
load). For comparison, before the content-protocol fixes EVERY run died at
1600-1800 frames.

ONE CRASH SITE CAUGHT under gdb, on the AUDIO PUMP THREAD (thread 12), not the
main thread:

    HostLockFor (kernel_sync.cpp:38)   <- inside an unordered_map lookup
    RtlEnterCriticalSection
    sub_825EA7D8
    AudioPump (xaudio_null.cpp:289)

IMPORTANT -- DO NOT 'FIX' kernel_sync.cpp ON THE STRENGTH OF THIS BACKTRACE.
That code is correct on inspection: the table has its own mutex, and the
recursive_mutex values are held by unique_ptr, so a rehash cannot invalidate a
reference already handed out. A hash map is a sensitive DETECTOR of heap
corruption arriving from elsewhere. Treating the detector would hide the cause.

A separate crash was caught earlier on the MAIN thread in sub_824961D0: an
indirect call through an object whose vtable pointer was a heap address rather
than an image one. Whether these are one bug or two is NOT established.

NOTE ON INSTRUMENTS: a run under gdb reached 8580 frames without crashing, so
gdb's timing perturbs whatever race this is. Reproducing it needs the core
file rather than a live gdb session.

This is distinct from catalog #44 (an intermittent _purecall panic) unless
something links them. Do not merge them on the strength of both being
intermittent.

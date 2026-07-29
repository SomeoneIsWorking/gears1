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

### Note (2026-07-29)
RESOLVED IN PART, AND MY HYPOTHESIS IN THIS ENTRY WAS WRONG.

I wrote that both crash sites 'smell like HOST-heap corruption' and that a hash
map is a sensitive DETECTOR of corruption arriving from elsewhere. The warning
not to patch kernel_sync.cpp was right. The DIAGNOSIS was wrong.

AddressSanitizer (runtime only, gears_ppc deliberately uninstrumented) caught it
in the act. The map is not corrupted -- it is DESTROYED, and then used:

  heap-use-after-free, READ of size 8, thread T12
    HostLockFor (kernel_sync.cpp:38) <- RtlEnterCriticalSection <- guest code
  freed by thread T14:
    ~unordered_map <- __run_exit_handlers <- exit
    <- GuestBugCheck (kernel_misc.cpp:414) <- KeBugCheck
    <- sub_828D2FB8 (the title's fatal handler) <- sub_828D0790 (_purecall)

GuestBugCheck called std::exit() from ONE guest thread while about nineteen
others were still running. std::exit runs the atexit handlers and destroys every
function-local static -- including that critical-section table -- and the other
threads kept calling through it. That is undefined behaviour by construction,
not a race that could be ordered away, and it is derivable statically: the table
is a function-local static and exit() destroys those.

FIXED with _Exit (runtime/fatal_exit.h): terminate now, run no handlers, destroy
no statics. Measured: 2 of 4 pre-fix runs reported the use-after-free; 5 of 5
post-fix runs reported ZERO heap errors of any kind.

THE INSTRUMENT WAS VALIDATED BEFORE BEING BELIEVED, which matters because a
sanitizer that reports nothing and a sanitizer that is not watching produce the
same output. GEARS_ASAN_SELFTEST=1 performs one deliberate out-of-bounds read
after the 4 GiB reservation is up, and it reports. So the five silent runs are a
real negative.

AND THIS LINKS #46 TO #44, WHICH I EXPLICITLY REFUSED TO DO WITHOUT EVIDENCE.
The freeing stack names the chain: _purecall (sub_828D0790) -> the title's fatal
handler -> KeBugCheck -> exit. So the PRIMARY fault is catalog #44's pure-virtual
call, and what this entry was chasing was a SECOND crash that the first one
caused and then buried. Keeping them separate was right until there was
evidence; there now is, and it is a stack trace rather than a coincidence of
timing.

WHAT REMAINS: the pure virtual call itself, which is #44 and is untouched by
this. It now reports cleanly instead of being masked.

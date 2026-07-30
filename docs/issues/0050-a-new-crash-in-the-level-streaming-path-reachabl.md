---
id: 50
title: A new crash in the level-streaming path, reachable only now the campaign loads
status: open
symptom: SIGSEGV at guest 0x2ac288ec inside sub_823ED7E0+0x52b3 at ~3420 frames, in the level-streaming path, on some runs but not others
tags: crash,streaming,level-load,nondeterministic,post-45
created: 2026-07-30
updated: 2026-07-30
---

REACHABLE ONLY BECAUSE #45 IS FIXED. With the checkpoint restore working the
title loads sp_prison_p and its streaming packages, and a crash appears in code no
run had ever reached before.

The fault reporter names it:

    signal: SIGSEGV
    address: guest memory at 0x2ac288ec (host 0x7f9252c288ec)
    __imp__sub_823ED7E0+0x52b3
    __imp__sub_823F0520+0x2d73
    sub_823EC880+0x551
    __imp__sub_823CDF00+0x47e
    sub_823CFEC0+0x5d2
    sub_823CEAB8+0x301e
    __imp__sub_82428238+0x8ef
    __imp__sub_822180F8+0x4fc
    __imp__sub_82218E10+0x5fc
    __imp__sub_82218F98+0x747
    _xstart

Reached 3420 frames. These are entirely different functions from #45, whose chain
ran through sub_824961D0 and other 0x8249xxxx code; this is 0x823Exxxx and
0x823Cxxxx.

IT IS NONDETERMINISTIC, which is the first thing to pin down. Three runs of the
same repro after the fix: two reached 8640 frames and were still running when the
300-second timeout stopped them, with zero faults; the third crashed at 3420. So
unlike #45 this is not a fixed point in the timeline, and a single clean run proves
nothing about it.

THE ADDRESS IS NOT YET EXPLAINED. 0x2ac288ec is a guest address about 700 MB in,
past the console 512 MB of physical memory, and well below the image at 0x82000000.
A pointer landing there is either garbage or a large offset applied to something
small.

WHAT NOT TO CARRY OVER. #45 spent a long time on a use-after-free reading that was
wrong, and several of its probes reported coincidences as evidence -- most notably
freed-while-cached matches that were really just a 192-byte pool size class being
recycled constantly. None of that reasoning applies here; this crash shares no code
with it.

FIRST STEPS, in order:
  1. Run the repro several times and record how many crash and at what frame
     counts. Without that denominator nothing else is interpretable.
  2. Check whether the faulting address is stable across occurrences.
  3. Read sub_823ED7E0 around +0x52b3 and see what it dereferences.

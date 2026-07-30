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

### Note (2026-07-30)
DENOMINATOR, AND BOTH CRASHES ARE THE SAME BUG.

SEVEN attempts on one binary, same repro, each capped at 170 seconds (stated
because a run that would fail later is counted clean by that cap):

  crashed: 2   (run 5 at 3480 frames, and the original at 3420)
  clean:   5   (4800-4860 frames each, still running when the cap stopped them)

So roughly 29 percent, n=7. Every run restored the checkpoint correctly, so the
#45 fix is stable across all seven.

THE TWO FAILURES ARE ONE BUG, not two. The original was a SIGSEGV; run 5 was an
abort from the checked indirect call. Their guest stacks are the same chain:

  run 5:    0x823edb50 <- 0x823f104c <- 0x823ec9dc <- 0x823ce030 <- 0x823cffe4
            <- 0x823ced9c <- 0x82428440 <- 0x82218250 <- ...
  original: sub_823ED7E0 <- sub_823F0520 <- sub_823EC880 <- sub_823CDF00
            <- sub_823CFEC0 <- sub_823CEAB8 <- sub_82428238 <- ...

Same functions, one frame apart. So the indirect-call check caught the same
corruption earlier and with far more detail than the segfault did, which is the
first time that instrument has paid for itself on a bug it was not written for.

WHAT THE MEMORY ACTUALLY CONTAINS. The bad call target was 0x3f5718e1, and the
object it came through looked polymorphic: r3 = 0x453c4a40 whose first word is
0x453c8280, read as a vtable. Decoding that "vtable" as IEEE-754 floats:

  0x453c7c20 =    3015.758      0x3f079632 =       0.530
  0x00000001 =       0.000      0x3f37d8d0 =       0.718
  0x3ec3ef14 =       0.383      0xbee71b43 =      -0.451
  0x445c3a3e =     880.910      0x4429740a =     677.813
  0x3e3d81fc =       0.185      0x3f77c173 =       0.968
  0x3e807aef =       0.251      0x3d0cab9d =       0.034
  0x3f733ce1 =       0.950      0x3e7f5e7f =       0.249
  0x440e0d36 =     568.206      0x438a4a66 =     276.581

and the bad call target itself is 0.84022 as a float.

That is not a vtable and never was. It is normalised values in [-1,1] interleaved
with magnitudes in the hundreds -- rotations and world-space translations. 0.38268
is exactly sin(22.5 degrees). So the block behind r3 holds ANIMATION OR TRANSFORM
DATA, and the code is walking it as an object with virtual functions.

WHICH NARROWS IT TO TWO SHAPES, and the difference is testable rather than
arguable: either the block was recycled out from under a live pointer (a lifetime
bug), or a pointer of one type is being used as another (a type confusion, e.g. an
array index or offset applied with the wrong stride). The float payload does not
distinguish them by itself.

NEXT: read sub_823ED7E0 around +0x52b3 and +0x5373 (the two faulting offsets) to
see how r3 is obtained -- whether it is loaded from a container the streaming path
mutates, or computed. And record whether the crashing addresses differ between
occurrences, which they did here (0x453c4a40 versus a SIGSEGV at guest
0x2ac288ec), suggesting the pointer is garbage rather than consistently stale.

### Note (2026-07-30)
THE LOOKUP HAPPENS ONCE PER RUN AND IS IN RANGE. HYPOTHESIS NOT SUPPORTED, NOT YET REFUTED.

Traced how the crashing object is obtained. sub_823ED7E0 reaches it through an
indexed table lookup (guest 0x823edab4..0x823edb4c):

    lwz  r11,772(r27)        ; a count -- checked only for > 0
    lwz  r11,768(r27)        ; a byte array
    lbzx r11,r11,r23         ; index = byteArray[r23]
    cmplwi cr6,r11,255       ; 255 is the "none" sentinel -- the ONLY check
    beq  cr6,exit
    lwz  r10,208(r19)        ; table base
    rlwinm r11,r11,4,0,27    ; index * 16
    add  r11,r11,r10
    lwz  r22,8(r11)          ; the object
    ...
    lwz  r11,0(r22)          ; its vtable
    lwz  r11,204(r11)        ; slot 51
    bctrl                    ; <- the crash

The index is validated against the sentinel and NEVER against the length, which
looked like the answer: an index past the end reads adjacent memory, and adjacent
memory is exactly where floats would come from.

MEASURED, and it does not support that. Instrumented the call site to report any
index at or past the count, with the total number of lookups printed periodically
so a zero is not bare:

    #50 streaming lookups: 1 seen, 0 out of range; highest index 3, lowest count 108

That line is identical across SIX clean runs. So this site is reached exactly ONCE
per run, with index 3 against a count of 108 -- nowhere near the edge. There is no
routine out-of-range lookup to find.

TWO CAVEATS, both mine to own:

  1. I have not captured this state on a CRASHING run. Every crash so far is at the
     same moment (3420, 3480, 3480 frames) and the same code, so the single lookup
     must be going wrong there -- but "in range in six clean runs" says nothing
     about the run that fails. The measurement I actually need is this line from a
     crashing run, and I do not have it.

  2. THE RATE DROPPED AFTER I ADDED THE PROBE. Tally across one binary lineage: 2
     crashes in 13 attempts overall, but ZERO in the 6 runs since the probe went
     in. That may be chance at these numbers, or the probe may be perturbing
     timing. It is not evidence that the bug is gone and must not be read as such.

ALSO WORTH RECORDING: the probe first reported only the anomaly, so a clean run
printed "0 out of range" and nothing about how many lookups that covered -- and
then a version reporting every 20000th lookup printed NOTHING AT ALL, because there
is only one. Two rounds of the same mistake in one sitting, on a file whose own
comments warn about it twice. The rule that would have caught both: print the first
occurrence unconditionally, so the instrument proves it is alive before it is
trusted to report an absence.

STILL OUTSTANDING: a temporary compare against one return address sits in
CallGuestIndirect, on all 29,190 sites. It has not answered yet, so it stays for
now, but it is debt and it is recorded here so it is not forgotten.

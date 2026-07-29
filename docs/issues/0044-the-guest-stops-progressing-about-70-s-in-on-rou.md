---
id: 44
title: The guest stops progressing about 70 s in, on roughly two runs out of three
status: open
symptom: the audio pump stops at exactly 11250 callback invocations and VdSwap plateaus at 1860-1920 frames, while the process stays alive and the renderer keeps drawing; other runs sail past the same point
tags: hang,nondeterministic,guest,audio,blocker
created: 2026-07-28
updated: 2026-07-29
---

> ## STATE AS OF THE LATEST NOTE — READ THIS FIRST
>
> **This is a lost-update race on UE3's render-command ring, and it is the
> TITLE'S race, not one this port introduces.**
>
> **Measured:**
> - The ring is `FRingBuffer` at `0x82C0CB24`; there is exactly ONE in the image
>   (all 45 allocator call sites target it).
> - The consumer is always `guest-7` alone, across 8 runs.
> - The PRODUCER is entered by both the game thread (12+ call sites) and the
>   rendering thread (**exactly one**: `0x82327d4c`, a `FlushCommand` from
>   `sub_82327BC8`).
> - The commit is a **non-atomic read-modify-write** on the ring's `+8`, done by
>   the caller, confirmed at three sites — each immediately preceded by an
>   `lwsync`. A release fence with no atomic RMW is a single-producer publish.
> - `bIsWriting` (+16) is stored and **never loaded**: it guards nothing.
>
> Two producers plus a non-atomic commit = one clobbers the other, `WritePointer`
> lands mid-command, and the consumer reads a header that is not one. It then
> advances `ReadPointer` by whatever the bogus `Execute()` returns, so it never
> recovers. That is the observed corruption and it explains the intermittency.
>
> **The route is now MEASURED** (guest backtrace at the enqueue):
>
> ```
> sub_82444EF0  the drain loop
>   --direct-->   sub_82444DB0        (bl 0x82444db0, return 0x82445004)
>     --INDIRECT--> sub_8221B378      <-- the one edge the call graph could not see
>       --direct--> sub_8221B670
>         --direct--> sub_8221D3A8
>           --direct--> sub_82327B60
>             --direct--> sub_82327BC8   enqueue FlushCommand (0x82327d4c)
> ```
>
> So the drain loop reaches the producer through EXACTLY ONE indirect dispatch,
> at the top of the chain; every edge below it is a direct call. That is why a
> whole-image direct graph found no path while the positive control passed — the
> break was at the first hop, not somewhere deep.
>
> **Still not established:** why the console survives this — plausibly Xenon's 6
> threads on 3 cores never truly overlap the RMW, where this port runs them in
> parallel.
>
> **DO NOT serialise the guest's commit yet.** The race being the title's means
> there is nothing of ours to stop doing; if the topology explanation holds,
> serialising becomes a defensible PORT decision rather than a bandaid — but that
> has to be established first.
>
> **Superseded below:** the `_purecall`/`KeBugCheck`/`exit` chain was a SECOND
> crash caused by this one and is FIXED (see `runtime/fatal_exit.h`); the
> "host-heap corruption" reading was wrong (the map was destroyed, not corrupted);
> and "no call site tests thread identity" was over-claimed.



Found while trying to verify an unrelated change, and the way it was found is
the lesson.

WHAT IT LOOKS LIKE: the pump's callback count freezes at 11250 -- exactly 60 s
of pumping at 187.5 Hz -- and VdSwap stops advancing past ~1860 frames. The
process does NOT die: checked at 30/60/75/90/105 s, alive throughout, and the
renderer keeps producing frames and log output afterwards. So the guest stops
making progress while the host side carries on.

Timing puts it right around the scripted input at 60.0-60.3 s, which is the A
press that starts the campaign. That is a suspicion from the clock, not a
finding.

HOW OFTEN: three identical 120 s runs gave 18750 / 11250 / 11250 pump calls and
2580 / 1860 / 1860 VdSwap frames. So roughly two in three, and the stalled runs
land on the SAME numbers, which says the stall point is deterministic even
though hitting it is not.

I HAD THIS WRONG FIRST, and the mistake is worth recording. I had just added a
backlog clamp to the audio pump, and comparing one run with it against one run
without it, I concluded the clamp caused a 3.5x throughput regression and
reverted it. Then the REVERTED build stalled identically. The clamp was never
the cause; I had compared two samples of a bimodal distribution and read the
difference as an effect. Two runs are not a control when the thing under test
is nondeterministic.

WHAT IS NOT KNOWN: where it is stuck. Three attempts to attach gdb during the
stall produced no backtrace at all (empty output, attach appears to fail while
the process is in this state), which is itself a clue worth chasing -- a
process that cannot be ptraced is usually in an unusual state. The next attempt
should try attaching earlier and staying attached, or launching under gdb from
the start rather than attaching to a stalled process.

WHY IT MATTERS BEYOND ITSELF: every measurement in this project that runs past
70 s now has a two-in-three chance of measuring a stalled guest instead of a
running one. The -O2 numbers recorded in issue #43 came from runs that did get
through, but any future comparison has to check that both arms actually
progressed before comparing anything. That check is cheap: the last VdSwap count
and the last pump call count.

### Note (2026-07-29)
DIAGNOSED, AND IT WAS NEVER A HANG. TWO SEPARATE FAILURES WERE BEING COUNTED AS ONE.

A stall detector now watches each subsystem's progress independently and, when
one stops, reports what every guest thread was doing (runtime/wait_probe.cpp).
Pointing it at this issue found that "the guest stalls" was two things:

FAILURE 1 -- THE TITLE BUG-CHECKS. The process was not hanging, it was EXITING.
The last line of a stalled run's log is an unimplemented-import abort on
KeBugCheck, the console's kernel panic: the guest itself decided something was
unrecoverable. Identical registers every time (r3=0, r4=0x82b8f4a0, r8=2).

    [kernel:error] THE TITLE BUG-CHECKED: KeBugCheck(code 0x0)

KeBugCheck and KeBugCheckEx are now implemented (kernel_misc.cpp): they report
the code, render any parameter that points at a string in the image, and exit.
That turns an anonymous abort into a named panic. WHY the title panics is now
the open question, and it is a much better question than the one this entry
started with.

FAILURE 2 -- AUDIO STOPS WHILE DRAWING CONTINUES. A separate run showed the pump
frozen at 11250 invocations with NO bug check and the renderer still drawing
3-draw frames at full rate. So the title's audio pipeline can stop on its own,
without the process dying and without the renderer noticing.

THE PART OF THIS ENTRY THAT WAS WRONG: it said "the process stays alive, checked
at 30/60/75/90/105 s". That observation was real but came from a run that did not
fail -- the healthy third of the distribution. I generalised it across all the
stalled runs without checking, which is the same mistake as the clamp
retraction recorded above: reasoning about a bimodal system from whichever
sample was to hand.

INSTRUMENT NOTES, both learned by the detector being wrong first:
- Its original progress signal was "any guest thread entered the kernel". That
  never fires: threads that poll (a 30 ms timed wait, a 500 us poll loop) keep
  entering the kernel forever, so a completely stuck guest still ticks. Kernel
  calls are now REPORTED as a description of the stall -- nothing running at all
  versus something spinning -- and progress is measured as subsystem work.
- A single global progress signal called failure 2 healthy, because drawing was
  progressing. Channels are watched independently for exactly that reason.
- Validated against a case that must trigger: GEARS_CP_STALL_MS=15000 with a
  short threshold produces the full report -- per-thread wait sites, durations,
  blocked-versus-running counts, and the recovery line when drawing resumes.

Frequency has also moved: 2 of 3 runs failed earlier today, 0 of 6 in the last
two batches, on a tree whose only behavioural change since is KeBugCheck. That
is not a fix and must not be read as one; it is more evidence the trigger is
timing-dependent.

### Note (2026-07-29)
THE PANIC IS A RE-ENTRANCY GUARD, SO SOMETHING FAILS SILENTLY BEFORE IT.

Static analysis of the image, which does not need the intermittent repro:
KeBugCheck has fifteen call sites, and their codes separate them cleanly.

    code 244  x7  in ppc_recomp.98.cpp   (returns 0x82614E80, 0x82615C00,
                                          0x826164E8, 0x826167D8)
    code 30   x1  returns 0x828D808C
    computed  x2  returns 0x828DE4AC
    code 0    x5  returns 0x828D3008 and 0x828D30B0

Ours is code 0, so it is one of the last five, all inside sub_828D2FB0. Reading
that code, the shape is unmistakable:

    r11 = load32(0x82BC9E18)
    if (r11 != 1) goto carry_on
    KeBugCheck(0)
  carry_on:
    store32(0x82BC9E18, 1)

That is a re-entrancy guard on a fatal-error path: the first entry sets the flag
and proceeds, and a SECOND entry panics. So the bug check is not the failure --
it is the title's fatal handler being entered twice. Something goes wrong,
the handler runs, and then something goes wrong again while it is running.

THE FIRST FAILURE IS SILENT. The 25 log lines before the panic are unbroken
3-draw frames at a loading or menu screen -- no warning, no unimplemented
import, no wait-site anomaly. Whatever the handler was called for produced no
trace on our side at all, which is itself informative: it is not a missing
import and not a kernel primitive misbehaving, or we would have logged it.

WHAT WOULD LOCALISE IT, and why it is not done here: sub_828D2FB0 has NO direct
callers anywhere in the recompiled image, so it is reached indirectly -- a
function pointer, a vtable, or a registered handler. The project's HLE override
seam works by defining a strong sub_<addr> that wins at link time, and that
cannot intercept an indirect call, because indirect calls go through
ppc_func_mapping.cpp straight to __imp__sub_<addr>. Instrumenting this needs
either a hook in the mapping table itself or a trace on the handler's
REGISTRATION (find what installs it: RtlSetUnhandledExceptionFilter and the
exception-dispatch imports are the first places to look).

WHAT IS BUILT NOW: KeBugCheck reports the link register, which names which of
the fifteen sites fired, and renders any residual register that points at a
string in the image. That distinguishes the code-0 sites from each other and
from the code-244 cluster. NOT YET SEEN FIRING -- the bug check has not
reproduced since it was added (0 of 6 runs), so the LR reporting is written and
compiled but unobserved.

### Note (2026-07-29)
TWO OF MY OWN CLAIMS IN THIS ENTRY WERE WRONG. BOTH CORRECTED, WITH EVIDENCE.

WRONG CLAIM 1: "the HLE override seam cannot intercept an indirect call, because
indirect calls go through ppc_func_mapping.cpp straight to __imp__sub_<addr>".
It does not. PPCFuncMappings holds `sub_X` -- the WEAK ALIAS -- so a strong
`sub_X` wins at link time for indirect calls exactly as it does for direct ones.
Verified with nm on the linked binary: sub_828D2FB0 resolves to the override's
object, at a different address from __imp__sub_828D2FB0. I had reasoned about
the table's contents instead of reading them.

WRONG CLAIM 2: "the five code-0 sites are all inside sub_828D2FB0". They are not.
An override on sub_828D2FB0 caught NOTHING across two runs that bug-checked
anyway -- which is how the error surfaced. Listing every function boundary in
the region shows the sites split across sub_828D2FB0 (lines 13064-13219) and
sub_828D2FB8 (13220-13371), and BOTH reach a call whose return address is
0x828D30B0. Two entry points eight bytes apart into the same body, and whatever
registers this handler registers the +8 one. My earlier analysis took the last
function boundary before the first call site and assumed it covered the rest.

WHAT IS NOW CONFIRMED RATHER THAN INFERRED: the link-register reporting added
last commit fired twice and named the site both times --

    THE TITLE BUG-CHECKED: KeBugCheck(code 0x0), called from 0x828d30b0

so the panic really is a code-0 site, and it really is the re-entrancy guard.

A RED HERRING, recorded so nobody chases it: 35 "wait on unknown handle
0xf8000040" errors appear right after the panic and in no healthy run. They
start on the line AFTER the bug check, so they are threads unwinding through a
handle table being torn down at exit -- aftermath, not cause.

STILL NOT SEEN: the first entry into the handler. The trace now covers both
entry points, but the bug check has not reproduced in the three runs since
(0 of 3, on top of 0 of 6 before). The instrument is in place and unexercised,
which is an honest state to leave it in -- it costs nothing until the failure
happens, and it will name the first failure when it does.

### Note (2026-07-29)
THERE IS ONLY ONE FAILURE HERE. "FAILURE 2" WAS ME MISREADING A LOG. RETRACTED.

This entry claimed a second, separate failure: "audio stops while drawing
continues", from a run whose pump report froze at 11250 invocations while the
renderer kept going. The pump did not stop. Reading the SUBMISSION counter
instead of the pump's own periodic line shows that same run reached 12000
submitted frames, near the end of the log. The pump had merely slowed below one
report per remaining minute -- which is issue #43, already known and already
measured, not a new failure.

The tell was there and I did not use it: the stall detector, watching audio as
its own channel, stayed SILENT through that run. It was right. A subsystem
progressing slowly is not a subsystem that stopped, and the instrument made
exactly that distinction while I was reading a missing log line as a dead
pipeline. That is a point in the detector's favour -- specificity, not just
sensitivity.

RECLASSIFIED, by bug check rather than by the pump-count heuristic I had been
using (nine runs, 120 s each):

    bug check:      3 of 9
    reached ~12000 audio frames / ~1900 swaps:  8 of 9
    reached ~19000 audio frames / ~2640 swaps:  1 of 9

So the bug check happens in about a third of runs, and how far the guest gets
varies independently of it -- the runs that bug-checked reached the same point
as several that did not. The earlier "2 of 3 runs stall" figure counted
slow-but-healthy runs as failures.

THE ENTRY'S TITLE IS NOW WRONG. The guest does not stop progressing; it
occasionally panics, and otherwise runs at a rate that varies with machine load.
Read the title as "the title bug-checks intermittently".

WHAT THIS MEANS FOR MEASUREMENT: "pump calls = 11250" is not a failure signal
and must not be used as one. The signals that mean something are the bug-check
line, the stall detector's report, and the submission counters. I used a proxy
because it was easy to grep, and it was wrong three times in a row.

### Note (2026-07-29)
CAUGHT THE FIRST ENTRY. THE RE-ENTRANCY-GUARD READING WAS WRONG TOO.

    [fatal:error] the title entered its fatal handler (sub_828D2FB8) from
      0x828da088: r3=0x3 r4=0x1 r5=0x0 r6=0x411dfcb0, re-entry flag 0 --
      this entry is the first
    [kernel:error] THE TITLE BUG-CHECKED: KeBugCheck(code 0x0), called from
      0x828d30b0

The handler panics on its FIRST entry, with the re-entry flag still 0. So the
guard I described in this entry is not what fires -- that guard is a different
call site (return address 0x828D3008). The one that actually fires, at
0x828D30B0, is:

    sub_828D30D8(...)                  // some finishing step
    r11 = load32(r31 + 164)
    if (r11 != 0) return               // normal exit
    KeBugCheck(0)                      // the field is ZERO -> panic

So the handler runs, does its work, and then panics because a field in its own
frame is zero. Not a double fault. A single failure whose handler cannot
complete.

THE CALLER CHAIN IS NOW NAMED. sub_828D9FD8 reaches 0x828DA080 and does:

    li r3, 3
    bl sub_828D3118        // -> the fatal handler, entered at +8

r3=3 is the only argument it sets; r4/r5/r6 in the trace are residual. So the
title is deliberately taking a fatal exit with code 3, not crashing into one.

WHAT IT DOES IMMEDIATELY BEFORE IS THE LEAD: it fills a structure at r1+80 with
pointers to r1+96 and r1+176, then calls sub_826138E8 and sub_826139B8. That
shape -- an argument block pointing at a buffer and a cursor, two calls, then a
fatal exit -- is what formatting an error message looks like. If it is, the
message names the failure outright.

ARMED FOR NEXT TIME: the fatal trace now scans the guest stack around the
handler's frame for printable runs and reports them as candidates (they are on
a stack, so some will be coincidence -- the log says so). It has not fired yet:
five more runs after adding it were all clean. The panic came in run 2 of the
previous batch of five, so the rate is well under the one-in-three this entry
estimated from nine runs.

STILL UNKNOWN: what sub_828D9FD8 is and what makes it choose the fatal path. It
is reached from somewhere in ppc_recomp.133, near the code that formats. That is
the next thing to read.

### Note (2026-07-29)
THE WHOLE CHAIN IS NOW MAPPED, AND IT LOOKS LIKE AN EXIT PATH, NOT A CRASH PATH.

Read statically, so none of this depended on catching the intermittent panic:

  sub_828D0790          runs a registered handler through a pointer if one is
                        set, then sub_828DA0A8(25), then sub_828DA088(0,1),
                        then falls into ->
  sub_828D9FD8          if a global flag bit is set, memsets a 2624-byte buffer
                        at r1+176, builds a descriptor at r1+96 and calls two
                        formatting routines; then UNCONDITIONALLY does
                        `li r3,3; bl sub_828D3118` ->
  the fatal handler     does its work and panics because a field in its own
                        frame is zero.

The important structural fact: the fatal exit at loc_828DA080 is reached
whether or not the message is formatted. sub_828D9FD8 has no path that returns.
It is not deciding anything -- it is the death itself, with an optional report.

WHERE THE DECISION IS: above sub_828D0790, which has NO direct callers anywhere
in the image and is reached only through the function-mapping table. It is now
traced the same way the handler is.

A HYPOTHESIS WORTH TESTING BEFORE ASSUMING A GUEST BUG: this shape -- run a
registered handler, report, terminate -- is what a C runtime's exit path looks
like, not what a crash looks like. If the title is deliberately ENDING (quitting
to dashboard, failing an init check and shutting down cleanly), then the
KeBugCheck is not the title crashing but the title's shutdown running into
state the runtime never set up: the handler panics because a field is zero, and
a field being zero is exactly what an unimplemented setup path leaves behind.
That would make this OUR defect rather than the guest misbehaving, and it would
explain why the failure is intermittent and leaves no trace before it.

Stated as a hypothesis. What settles it is the link register on
sub_828D0790, which names the decision-maker; the trace for that is now in
place and unexercised.

RATE: four more clean runs after adding the trace, on top of five before. The
"one in three" figure earlier in this entry came from nine runs classified by a
proxy that turned out to be wrong, and the honest current estimate is closer to
one in eight. Do not size an investigation off it.

### Note (2026-07-29)
LAST NOTE'S HYPOTHESIS IS FALSIFIED, STATICALLY, BEFORE ANYTHING WAS BUILT ON IT.

I suggested the handler might be panicking because a field was zero that the
runtime never set up -- making this our defect. It is not. Reading the wrapper
that calls the handler:

    sub_828D3118:  r5 = 0;  r4 = 1;  goto sub_828D2FB8
    sub_828D3128:  r5 = 1;  r4 = 0;  r3 = 0;  goto sub_828D2FB8

The handler stores that r5 at frame+164 and panics precisely when it is zero. So
the zero is HARDCODED BY THE TITLE, in the wrapper it chose to call. Two
wrappers onto one handler: one that ends the process and one that returns. The
bug check is the designed "must not return" tail of a deliberate terminate, not
state we failed to populate.

That also explains the shape that prompted the hypothesis. It really is an exit
path -- I was right about that -- but the panic at the end is intended, not a
symptom of the exit going wrong in our runtime.

CONSEQUENTLY THE LOG LINE WAS MISLEADING and is fixed: "THE TITLE BUG-CHECKED"
reads as a crash. It now says the title called KeBugCheck from the tail of its
own terminate path, so it MEANT to end, and names the remaining question. Only
code 0 from 0x828d30b0 is described that way; any other code or site is still
reported as unrecognised, because nothing has been seen there and assuming
would be the same mistake in the other direction.

THE QUESTION IS UNCHANGED AND NARROW: what calls sub_828D0790. Everything from
there down is unconditional and now understood. That trace is armed.

Worth noting for whoever picks this up: four models of this failure have been
wrong so far -- a clamp regression, a live-but-stuck process, a double-fault
re-entrancy guard, and unpopulated runtime state. Every one was a plausible
reading of disassembly or logs that the running system contradicted. The two
findings that held up came from the trace firing and from reading the wrapper's
four instructions. Prefer the small decisive read over the compelling story.

### Note (2026-07-29)
THE DECISION POINT IS FOUND. IT IS A VIRTUAL CALL, AND THAT CHANGES THE QUESTION.

    [fatal] the title began shutting down (sub_828D0790) from 0x82444f7c:
            r3=0x40274748 r4=0x0 r5=0x0 r6=0x4

0x82444f7c is the return address of a `bctrl` inside sub_82444EF0, and the code
around it is an ordinary virtual dispatch out of an object walk:

    r30 = load32(r31 + 20)      // current object in a list
    r11 = load32(r30 + 0)       // its vtable
    r11 = load32(r11 + 4)       // slot 1
    mtctr r11 ; bctrl           // call it, this = r30

So nothing "decided" to quit in the sense of a check failing. The title was
walking a list of objects calling a virtual method on each, and one of those
calls arrived at the CRT terminate routine.

sub_82444EF0 IS ON THE PER-FRAME PATH. It appeared in the very first thread
census of this session, in the chain sub_824A5170 -> sub_82487510 ->
sub_824A42A0 -> sub_82444EF0. This is a routine object iteration that runs
constantly, which fits the failure being rare and timing-dependent.

TWO POSSIBILITIES, AND THEY WANT OPPOSITE FIXES:
 (a) slot 1 of that object's vtable legitimately IS a shutdown method, and
     something upstream asked for shutdown -- a title-level decision we have
     not found yet.
 (b) the object is not what the caller believes. A stale, freed or
     never-constructed object would have a vtable pointer that is garbage, and
     the dispatch would land in whatever that garbage addresses. Landing in the
     CRT exit path would then be coincidence, and the real defect is whatever
     corrupted or freed the object -- which, given everything else here works,
     would most likely be OUR memory or lifetime handling.

ONE WORD OF GUEST MEMORY SEPARATES THEM: the vtable pointer at the object's
offset 0. If it points into the image it is a real vtable; if it does not, the
object is corrupt. The trace now reads it and says which, along with the value
of slot 1. Armed, not yet fired.

METHOD NOTE: this was caught on run 4 of a batch of 6, after nine consecutive
clean runs. Batches, not single runs, are the right shape for a one-in-eight
failure -- and every trace in this chain was placed from static reading first,
so each catch produced an answer rather than another guess.

### Note (2026-07-29)
IT IS A PURE VIRTUAL CALL. THAT IS THE ANSWER, AND IT IS A LIFETIME BUG.

sub_828D0790 is _purecall. Searched the loaded image (13.5 MB dumped from a
running process) for its address:

    1837 occurrences, every one 4-byte aligned
    each sits inside a run of code pointers -- vtables
    often in CONSECUTIVE slots (0x82055708, 0x8205570c, 0x82055710 all hold it)

One address shared by 1837 vtable slots, which terminates when called, is what
a compiler emits for PURE VIRTUAL slots: a single stub that dies if anyone
dispatches to one.

So nothing decided to quit and nothing is corrupt in the "garbage pointer"
sense. The title made a virtual call on an object whose vtable is an ABSTRACT
BASE'S. The classic causes are exactly three: a virtual call during
construction, a virtual call during destruction, or a call on an object another
thread has already destroyed.

WHY THIS FITS EVERYTHING SEEN: the call comes out of sub_82444EF0, a per-frame
walk over a list of objects calling virtual slot 1 on each. That runs constantly,
so a window of a few instructions during some object's construction or teardown
would be hit rarely and unpredictably -- which is precisely the one-in-eight,
timing-dependent behaviour that made this so hard to pin down.

WHY IT MAY STILL BE OURS: a lifetime race in the title is latent on hardware and
can be exposed by a runtime whose threading differs. Two differences are already
recorded in this project -- processor numbers that alias, so two host threads
can claim one guest CPU concurrently (catalog #41/#42), and IRQL modelled per
thread rather than per processor. Either can widen a window the console kept
shut. That is a hypothesis, and the way to test it is to find WHICH object.

THE NEXT STEP IS NARROW AND ALREADY ARMED: the trace reads the object's vtable
pointer. That vtable identifies the class. Then the question becomes who
constructs or destroys that class while the per-frame walk is running, which is
answerable by tracing its constructor and destructor.

LABELLING FIXED: both the trace and the KeBugCheck line called this a
deliberate terminate, which was my reading two notes ago and is wrong. A
pure virtual call is a bug, not an exit. The messages now say so and point at
each other.

### Note (2026-07-29)
THE OBJECT IS NAMED, AS FAR AS THE IMAGE ALLOWS. IT IS A SECONDARY BASE.

Caught with the vtable read on run 9 of a batch:

    PURE VIRTUAL CALL (_purecall at sub_828D0790) from 0x82444f7c:
      r3=0x4029ae44
      the object it was called on (0x4029ae44) has vtable 0x820bc8b4
      (a real vtable), slot 1 = 0x828d0790

So the vtable pointer is genuine -- not garbage, not a freed-memory pattern --
and slot 1 really is _purecall. Walking the image backwards from it, the vtable
BLOCK starts at 0x820bc818 and the object's pointer sits 39 slots inside it:

    slots  0, 1, 4..11   _purecall
    slots  2, 3, 12..38  real methods
    slot  39             = the address the object stores  <-- secondary base
    slots 40, 41         _purecall   <-- slot 1 from the object's pointer

A pointer landing 39 slots into a vtable block is the signature of MULTIPLE
INHERITANCE: one contiguous block holds a vtable per base, and a pointer to a
secondary base subobject points into the middle. So the per-frame walk is
dispatching through a secondary base interface whose slot 1 is pure.

WHAT THAT NARROWS IT TO, and these are now the only two live explanations:
 (a) the object is mid-construction or mid-destruction, so its vtable pointer is
     still (or already) the abstract base's rather than the concrete class's;
 (b) the caller adjusted the pointer to the wrong base -- a cast whose offset is
     wrong -- and is calling slot 1 of an interface this object never
     implements.

(a) remains the better fit for a failure that is rare and timing-dependent; (b)
would be deterministic and would fail every frame.

WHERE IT STOPS FOR NOW: naming the concrete class needs whoever WRITES
0x820bc818/0x820bc8b4 into an object, i.e. the constructor. That address is not
built with a plain lis/ori pair anywhere in the recompiled code -- 753 sites use
the matching lis, none with a matching low half -- so it is loaded from a data
pointer, and finding it needs a search of the image's data for pointers TO the
vtable rather than a search of the code.

RATE, for whoever continues: this took 9 runs at 85 s. Shorter runs are better
value than longer ones -- the failure lands around 70 s, so anything past ~90 s
is spent waiting rather than sampling.

### Note (2026-07-29)
THE LIST IS A GLOBAL REGISTRY WITH 43 REFERENCING SITES. THIS IS THE TICKABLE-OBJECT SHAPE.

sub_82444EF0, the per-frame walk that makes the pure virtual call, iterates a
global structure at 0x82C0CB64 -- fields at +0, +8, +12 and +20 used as
start/end/cursor -- and the whole walk is gated on a flag at 0x82BFA380 being
non-zero. 43 sites across 14 translation units reference that same structure
(confirmed by matching the lis base, not just the offset).

A global registry that many classes touch, walked every frame, with a virtual
call per entry, is the shape of UE3's tickable-object list: objects add
themselves in a constructor and remove themselves in a destructor, and the
engine calls a virtual method on each one every frame.

THAT MAKES THE MECHANISM A TEXTBOOK ONE, and it is worth naming even though it
is not yet proven here. The classic pure-virtual-call in that design comes from
the registration outliving the object's concrete type: the base constructor
registers `this` before the derived vtable is installed, or the object is
destroyed without being removed from the list. Either way the walk dispatches
through a vtable that is the abstract base's, which is exactly what was
measured -- a genuine vtable, 39 slots into a multiple-inheritance block, with
_purecall in the slot being called.

STATED AS A HYPOTHESIS. What would confirm it: find which of the 43 sites APPEND
to the structure and which REMOVE from it, and check whether the appending one
runs in a constructor whose derived vtable is not yet installed. That is static
work and does not need the intermittent repro.

WHY THIS MATTERS FOR THE PORT rather than being the title's problem to have: if
registration and removal are correct on hardware, the window between them is
closed by the console's scheduling, and our runtime's known threading
differences -- processor numbers that alias so two host threads can claim one
guest CPU (#41, #42), IRQL modelled per thread rather than per processor -- are
the kind of thing that opens it. Confirming the mechanism first is what makes
that testable rather than speculative.

### Note (2026-07-29)
THE TICKABLE-REGISTRY HYPOTHESIS IS WEAKENED BY ITS OWN EVIDENCE. RECORD BEFORE IT HARDENS.

Sorting the 43 references to the global at 0x82C0CB64 by enclosing function
gives 43 sites in 43 DISTINCT functions -- exactly one each. A registration
list would not look like that. It would show a small number of functions doing
the work (an append in a constructor, a removal in a destructor, a walk in the
tick) called from many places, not forty-three different functions each
touching the container once.

Two of those functions are slots 17 and 18 of the very vtable the failing object
carries (sub_8221BD60 and sub_8221C1B8 at 0x820bc85c and 0x820bc860). So the
global is state that this class's own methods manipulate, not a registry shared
by unrelated classes registering themselves.

That does not disprove the tickable-object reading, but it removes the evidence I
offered for it. The honest position is that the structure is a container this
subsystem owns and walks, and its exact role is unknown. I am recording this
immediately because the previous note made the UE3 pattern sound established,
and a pattern that FEELS right is what produced four wrong models of this bug
already.

WHAT SURVIVES, all measured rather than inferred:
  - a pure virtual call, from a per-frame walk (sub_82444EF0)
  - on an object whose vtable is genuine and is a secondary base, 39 slots into
    a multiple-inheritance block at 0x820bc818
  - the vtable's owning class manipulates the same global the walk iterates
  - the failure is rare, timing-dependent, and lands around 70 s

NEXT, and it should be a narrower question than the one I have been asking: read
sub_8221BD60 and sub_8221C1B8 -- the two vtable methods that touch the container.
They are the class's own interface to the list it lives in, so what they do to it
(insert? remove? mark?) says what the lifetime contract is, and therefore what
breaking it looks like.

### Note (2026-07-29)
THE CLASS IS IDENTIFIED: IT IS THE MOVIE PLAYER. THE TIMING FITS EXACTLY.

Reading the wide strings around the failing object's vtable (0x820bc818) settles
what it belongs to. Immediately before it, in address order:

    0x820bc578  'Startup.bik'      0x820bc590  'MGSLogo.bik'
    0x820bc5a8  'EpicLogo.bik'     0x820bc5fc..'ESRB_*.bik' (7 languages)
    0x820bc6e8  'Movies\\'          0x820bc718  'Subtitles'
    0x820bc740  'nomovie'          0x820bc758  'HOSOutro'
    0x820bc788  'PlayGearsMovieCommand'
    0x820bc7b4  'StopGearsMovieCommand'
    0x820bc7e0  'MovieEffects'     0x820bc7fc  'MovieVoice'
    0x820bc818  <-- the vtable the failing object carries

The vtable directly follows 'MovieVoice', in a block that is nothing but Bink
movie filenames, the Movies path, subtitle and audio-channel names, and the
Play/Stop console commands. The class is the title's full-screen movie player.

AND THAT EXPLAINS THE TIMING. The failure lands around 70 s, which is the
campaign-start transition -- exactly when a loading movie starts or stops. It
also explains what the log looks like just before: unbroken 3-draw frames, which
is what a movie or loading screen renders.

So the mechanism, now with every piece measured rather than assumed: the
per-frame walk dispatches a virtual on the movie player through a secondary base
interface, in a window where the object's vtable is the abstract base's --
during construction or destruction, which is to say at a movie start or stop.

A CLEAN FALSIFIABLE TEST EXISTS AND THE TITLE PROVIDES IT. 'nomovie' and
'NOMOVIE' are command-line switches this very code reads. If movies are
disabled and the panic stops, the movie player is confirmed; if it still fires,
this identification is wrong and should be discarded.

The runtime does not pass a command line to the guest today (main.cpp takes only
the XEX path and game directory), so the test needs that first. It is a small
addition and it is worth having anyway -- the title reads a dozen switches here
(ONETHREAD, NOSOUND, BENCHMARK, DUMPMOVIE, NOINI, SECONDS=, EXEC=) that are all
useful for bringing a port up.

NOTE ON WHAT THIS IS NOT: disabling movies would not be a fix. It would be the
experiment that confirms where the bug lives.

### Note (2026-07-29)
THE -nomovie EXPERIMENT IS RUNNING. FIVE CLEAN RUNS SO FAR, WHICH IS NOT EVIDENCE YET.

The runtime now hands the title a command line through launch data
(GEARS_CMDLINE), and the switch is verified to do something real: with no
switch the title opens EpicLogo.bik, ESRB.bik, MGSLogo.bik and Startup.bik;
with -nomovie it opens none, in every run of the batch.

Five runs with movies disabled, no panic.

WHAT THAT IS WORTH, stated before anyone reads it as a result. The observed
baseline is roughly 6 panics in ~48 runs, so p is about 0.125:

    P(0 panics in  5 runs | movies are irrelevant) = 0.51
    P(0 panics in 10 runs | movies are irrelevant) = 0.26
    P(0 panics in 20 runs | movies are irrelevant) = 0.07
    P(0 panics in 30 runs | movies are irrelevant) = 0.02

Five clean runs is MORE LIKELY THAN NOT even if the movie player has nothing to
do with it. It is worth nothing on its own. Twenty starts to mean something;
thirty would be convincing.

This is written down deliberately. Earlier in this same investigation I
concluded a backlog clamp caused a 3.5x regression by comparing one run against
one run, and had to retract it when the reverted build behaved identically. The
failure mode is reading a bimodal system off too few samples, and the defence is
computing the number BEFORE looking at the outcome, not after.

CONTINUE: accumulate -nomovie runs toward twenty, then thirty. A single panic
with movies disabled ends the experiment immediately and refutes the
identification, which is the outcome to watch for -- it is much more
informative, and much cheaper to obtain, than the null.

### Note (2026-07-29)
EXPERIMENT DESIGN IMPROVED, AND A BETTER ONE IS AVAILABLE.

Progress so far, tracked in scratch/exp/nomovie.tsv:

    nomovie:  0 panics / 8 runs
    control:  0 panics / 3 runs

The arms are now INTERLEAVED rather than run in blocks, because the 0.125
baseline was measured across a session with varying machine load and several
different builds, and a matched control removes both. Zero in three control
runs is unremarkable (P = 0.67 under the baseline), which is the point: at this
rate BOTH arms need twenty-plus runs, and grinding toward that is slow.

A SHARPER EXPERIMENT EXISTS. If the failure is a movie start/stop race, then a
run with MORE movie transitions should fail more often -- and the title has
'AttractMode.bik' plus an attract-mode loop that plays when it is left alone at
the title screen. A run with no input script should cycle movies repeatedly
instead of passing through the menus once.

That predicts something falsifiable and cheap: idling at the title screen should
raise the panic rate well above 0.125, and if it does not, the movie
identification is in trouble. Raising the rate also makes every subsequent
measurement cheaper, which is worth more than another five runs of the null.

Try that before accumulating further. A null obtained slowly is worth less than
a positive obtained quickly, and this is the rare case where the failing
condition can be asked for directly rather than waited for.

### Note (2026-07-29)
NEGATIVE EVIDENCE AGAINST THE MOVIE MECHANISM, AND A WEAKNESS IN MY OWN IDENTIFICATION.

The sharper experiment does not work as designed: idling at the title screen for
130 s cycles NO attract movie. The only .bik files the title ever opens are the
four boot movies -- Startup, MGSLogo, EpicLogo, ESRB -- and AttractMode.bik
never appears. The prediction was wrong, so that route to a higher failure rate
is closed.

Worse for the hypothesis, tracing file access across three runs puts the LAST
.bik access at line 118 of 4000-7700 -- two to three percent through the run,
during boot. The panic lands around 70 s, roughly fifty seconds after the movie
subsystem last touched a file. NO MOVIE IS LOADING ANYWHERE NEAR THE FAILURE.

That does not kill the class identification, but it does undercut the mechanism
I proposed: "a movie starting or stopping" cannot be it, because nothing is
starting or stopping then.

AND THE IDENTIFICATION ITSELF IS WEAKER THAN I PRESENTED IT. It rests on the
vtable at 0x820bc818 being ADJACENT in the data section to a block of movie
strings. Adjacency is suggestive; it is not ownership. Constants from unrelated
translation units land next to each other all the time. I wrote it up as "the
class is the title's movie player", and the evidence supports something more
like "the vtable sits in the same region of .rdata as the movie subsystem's
strings".

WHAT WOULD IDENTIFY IT PROPERLY: find the code that WRITES 0x820bc818 (or
0x820bc8b4) into an object -- the constructor. Earlier I looked for a lis/ori
pair building the address and found none, which means it is loaded from a
pointer in data. The search is for words in the image whose VALUE is
0x820bc818, then for code that loads from those addresses.

THE -nomovie ARM KEEPS ITS VALUE regardless: if disabling movies removes a panic
that happens fifty seconds after the last movie file access, that is a strong
and surprising result. Current tally, matched arms:

    nomovie:  0 panics /  8 runs
    control:  0 panics /  7 runs

### Note (2026-07-29)
LINKED TO #46 BY A STACK TRACE, and the masking is now removed.

AddressSanitizer, chasing the intermittent crash in #46, produced the chain that
ties them together:

    sub_828D0790 (_purecall)
      -> sub_828D2FB8   the title's fatal handler
      -> KeBugCheck
      -> GuestBugCheck (kernel_misc.cpp:414)
      -> std::exit
      -> ~unordered_map from __run_exit_handlers
      -> heap-use-after-free on another guest thread

So this pure-virtual call is the PRIMARY fault, and the crash #46 was chasing
was a second one that this one caused: exiting from one guest thread destroyed
the function-local statics that nineteen other running threads were still
calling through.

WHY THAT MATTERS HERE: the second crash was MASKING this one. The process died
in a hash-map lookup on the audio thread, which is where a debugger pointed and
where attention went. With the exit path fixed (_Exit, no handlers, no static
destruction -- runtime/fatal_exit.h) the pure-virtual call now reports cleanly
and dies where it actually failed.

Also recorded from the same run: the guest-side log line is
    [fatal:error] PURE VIRTUAL CALL (_purecall at sub_828D0790), from 0x82444f7c
which names a CALLER this entry did not previously have. sub_82444EF0 is on the
stack (ppc_recomp.60.cpp:23437). That is a concrete place to start, and it is
more than 'an object walk somewhere'.

One ASan run caught the same fault as a plain jump through a null vtable slot --
'SEGV on unknown address 0x000000000000, pc points to the zero page', with
r15 = 0x82444ef8 -- which corroborates the object's vtable being unusable rather
than merely abstract.

### Note (2026-07-29)
THE PURE VIRTUAL CALL IS A RING-BUFFER FRAMING FAILURE, NOT (NECESSARILY) A LIFETIME BUG.

sub_82444EF0 is UE3's RenderingThreadMain: it drains an FRingBuffer at
0x82C0CB24. Field map recovered and cross-checked against the allocator
sub_8221CBA8: +0 Data, +4 DataEnd, +8 WritePointer, +12 WriteEnd, +16
bIsWriting, +20 ReadPointer.

The faulting call is the command's virtual SLOT 1 (byte offset +4):
    r30 = ReadPointer ; r11 = [r30+0] ; r11 = [r11+4] ; bctrl   (lr 0x82444F7C)
    r29 = r3 ; ... ; ReadPointer += r29
That is FRenderCommand::Execute(), and its RETURN VALUE advances ReadPointer.
Ground truth for the class: sub_82445278 allocates 8 bytes, stores vtable
0x82106D58, whose slot +4 returns exactly 8 and whose slot +8 returns the UTF-16
literal 'FenceCommand'.

CONSEQUENCE: once ReadPointer lands off a command boundary it NEVER recovers,
because the loop advances it by whatever the bogus Execute() returned. A vptr
reading as a heap address (0x42babe80, as the probe reported) is what you get
when ReadPointer sits on a command's PAYLOAD rather than its header.

WHAT IS NOT ESTABLISHED, and this correction matters. The investigation
concluded that a virtual call during construction/destruction was RULED OUT,
because no render-command vtable has _purecall at slot +4. That reasoning is
near-tautological and was refuted on review: the scan RECOGNISES a command
vtable by slot +8 being a string-returning thunk, so a fully abstract base
vtable -- shape [real dtor, purecall, purecall] -- is structurally invisible to
it and the scan can only ever print zero. That shape exists in this image and IS
stored into objects. So a ctor/dtor race is NOT ruled out. What survives is the
weaker, direct evidence: the two enqueue sites read instruction by instruction
(sub_82445278, sub_8221BF50) each emit exactly ONE vptr store, with the commit
fenced after it.

RULED OUT BY MEASUREMENT: non-constant command sizes (every Execute returns a
small constant matching its enqueue-site allocation), dropped barriers (lwsync
is translated as an acq_rel fence, and PPC_LOAD/STORE_U32 are volatile so the
spin loops cannot be hoisted), and a runtime override sitting on an Execute
(none of the 13 overrides is one of the 55 Execute addresses).

FLAGGED, NOT BLAMED: stwcx. is translated as a value compare-and-swap rather
than a store-conditional, so it silently succeeds under ABA where the console
would fail the reservation.

THE THREE NEXT INSTRUMENTS, in order of value:
1. A thread-id assertion at the top of sub_82444EF0's loop and in sub_8221CBA8.
   FRingBuffer assumes ONE producer and ONE consumer; two threads in either is
   precisely this corruption, and this port has nineteen live guest threads.
2. Log slot +8 (DescribeCommand, which returns a literal string) before each
   Execute. The last good command name plus the byte offset of the divergence
   names the culprit directly.
3. Print [0x82C0CB24+0] and [+4] and check whether the bad vptr falls inside
   Data..DataEnd -- that would confirm the payload-misalignment reading.

### Note (2026-07-29)
MEASURED: TWO THREADS ENTER THE RING'S PRODUCER PATH. NOT YET SHOWN TO BE THE CAUSE.

Three instruments were added (runtime/guest_probes.cpp) and run:

  [ring] the render-ring drain loop entered by guest thread 'guest-7'
  [ring] drain loop starting: Data=0x40270000 DataEnd=0x402b0000
  [ring] the render-ring allocator entered by guest thread 'host'
  [ring:error] the render-ring allocator entered by a SECOND guest thread:
               'guest-7' after 'host'  ... (8 alternations in one run)

So the consumer is a single thread (guest-7, the rendering thread), but the
PRODUCER path sub_8221CBA8 is entered by BOTH the main thread ('host', which is
the title's game thread) and by guest-7 itself. FRingBuffer is documented -- and
written -- as single-producer/single-consumer.

WHAT THIS DOES **NOT** SHOW, and the distinction matters because it would be
easy to declare victory here: the _purecall did NOT fire on this run. The run
died at the save-load deserialise instead (catalog #45's deterministic path),
having survived all eight thread alternations. So two producers is a real,
measured HAZARD that breaks a documented invariant, but it is not a demonstrated
cause of #44. It may even be legitimate -- a render command enqueuing another
command is a shape UE3 does use.

TO PROMOTE IT TO A CAUSE, one of these is needed:
  - a run where _purecall fires AND the object pointer lands inside
    Data..DataEnd (0x40270000..0x402b0000 on this run). The probe now prints
    that verdict directly when the pure-virtual handler is entered.
  - or a demonstration that the two producers can interleave INSIDE the
    reserve/commit window (sub_8221CBA8 reserves, the caller stores the vtable,
    then the commit advances WritePointer). Overlapping that window is what
    would actually corrupt the framing; merely alternating between calls would
    not.

The instruments stay in the tree; they cost nothing and #44 is intermittent, so
the next occurrence will now carry its own diagnosis.

### Note (2026-07-29)
THE ATOMICS ARE RULED OUT FOR THIS BUG, BY MEASUREMENT.

sub_8221CBA8 (the ring allocator) and sub_82444EF0 (the drain loop) contain
ZERO lwarx / stwcx. / ldarx / stdcx. / lwsync / sync / eieio. Verified twice,
independently, by scanning the recompiled function extents. They use plain
loads and stores on the ring fields only.

That is safe under this port because PPC_LOAD_U32 / PPC_STORE_U32 are volatile
(ppc_context.h), so the producer's spin genuinely re-reads, and x86-64 TSO
supplies the ordering PowerPC would need lwsync for.

So whatever moves ReadPointer off a command boundary, it is NOT the atomic
translation. The place that remains is the drain loop itself: Execute()'s return
value is added straight to ReadPointer (lwz r11,20(r31); add r11,r11,r29; stw
r11,20(r31)), with the wrap handling against +12.

### Note (2026-07-29)
THE FRAMING MECHANISM IS IDENTIFIED: A NON-ATOMIC COMMIT, WITH TWO PRODUCERS.

Static analysis, re-derived independently on review, at two separate commit
sites (ppc_recomp.60.cpp:23988 for FenceCommand and ppc_recomp.15.cpp:14810
inside sub_8221BD60), so this is the general shape and not one site:

  - The ALLOCATOR (sub_8221CBA8) never advances WritePointer. The reservation
    lives only in the caller's own stack frame.
  - The COMMIT is a non-atomic read-modify-write on the ring's +8:
        lwz r11,8(ring) ; add r11,r11,size ; stw r11,8(ring)
    followed by clearing bIsWriting (+16).
  - bIsWriting is STORED (1 at reserve, 0 at commit) and NEVER LOADED by either
    function. Whatever it was meant to guard, it guards nothing in this build.

  Ring geometry confirmed against the runtime probe: the constructor allocates
  0x40000 bytes with Data = WritePointer = ReadPointer = buf and
  DataEnd = WriteEnd = buf + 0x40000, matching the measured
  Data=0x40270000 DataEnd=0x402b0000.

COMBINED WITH THE MEASUREMENT already in this entry -- that BOTH the game thread
and the rendering thread enter the allocator -- this is a lost-update race. Two
producers each read WritePointer, add their own size, and store; one clobbers
the other. WritePointer then points into the middle of a command, the consumer
reads a header that is not one, and ReadPointer advances by whatever garbage
Execute() returns. That is exactly the observed corruption, and it explains why
it is intermittent.

WHAT IS STILL NOT ESTABLISHED, and it decides whose bug this is: whether the
console also has two producers here. If UE3 genuinely enqueues from the
rendering thread, the non-atomic commit is a title bug that hardware happens to
survive; if our port introduces the second producer -- by running something on
the wrong thread -- it is ours. The next measurement is which CALL SITES the two
producers come from: log the caller (link register) at sub_8221CBA8 per thread,
and compare the rendering thread's callers against what UE3 is expected to
enqueue from its own thread.

Do NOT add locking around the guest's commit. If the second producer is ours,
the fix is to stop producing from that thread.

### Note (2026-07-29)
THE RENDERING THREAD HAS EXACTLY ONE PRODUCER CALL SITE.

Measured by logging the link register at the allocator per thread, as a set:

    producer 'host'    -- 12+ distinct call sites (0x824452cc, 0x8221bf94,
                          0x8257a5e0, 0x82453ff4, 0x8241c5d4, 0x8257a820,
                          0x82445454, 0x82546448, 0x82546648, 0x8221c1f8,
                          0x8258a798, 0x824a40f4, ...)
    producer 'guest-7' -- EXACTLY ONE: 0x82327d4c

That asymmetry is the useful part. The game thread enqueues from everywhere, as
expected; the rendering thread enqueues from a single place, inside
sub_82327BC8, reserving 112 bytes from the ring (li r5,112, base 0x82C0CB24),
behind the usual GIsThreadedRendering test.

sub_82327BC8 has three direct callers -- sub_82181F28, sub_821836E8,
sub_8218A008 -- and appears once as a data word at 0x82142A00, i.e. in a table,
so it is also reachable indirectly.

WHAT THIS DOES NOT YET SETTLE: whether those callers legitimately run on the
rendering thread (UE3 does enqueue from render commands in places) or whether
this port runs that code on the wrong thread. One call site is a small enough
target to answer by reading the three callers' call graphs, which is in
progress.

### Note (2026-07-29)
PROVENANCE SETTLED: THE SECOND PRODUCER IS THE TITLE'S OWN CODE ON THE TITLE'S OWN THREAD. NOT INJECTED BY THIS PORT.

Re-derived independently on review; the numbers below reproduced exactly.

  - 45 call sites of the allocator across 40 functions, and they ALL target the
    same ring: 36 build 0x82C0CB24 immediately, and the 9 that pass it in a
    register (r30/r18/r29 in sub_82428238, sub_824453F8, sub_824A3900,
    sub_824A3EF0) each resolve to that same address. There is exactly ONE render
    command ring in this image.
  - guest-7's single enqueue site 0x82327D4C is inside sub_82327BC8, which
    reserves 112 bytes and stores vtable 0x820E40B8 -- FlushCommand (its slot +8
    returns that UTF-16 literal). Cross-validated: FlushCommand::Execute
    (sub_82327E00) has exactly ONE direct caller in the image, the inline arm of
    that same macro, which is what the two arms of ENQUEUE_UNIQUE_RENDER_COMMAND
    look like.
  - Across 8 runs the consumer is ALWAYS guest-7 alone (8 'drain loop entered'
    lines, zero 'SECOND'), and the producers are only ever host and guest-7,
    ~30 alternations each corpus-wide.

THE STRONGEST STATIC EVIDENCE THAT ONE PRODUCER IS INTENDED, and it was missed
first time round: every commit read-modify-write is immediately preceded by an
lwsync (ppc_recomp.60.cpp:23980, .15.cpp:14804, .40.cpp:2735). A RELEASE FENCE
WITH NO ATOMIC RMW is exactly the shape of a single-producer publish. An earlier
note here said the commit path had no barriers at all; that was wrong -- the
barrier is there, it is just a fence rather than an atomic.

WHAT IS NOT ESTABLISHED, stated plainly because it is the whole remaining gap:
HOW the rendering thread reaches sub_82327BC8. A whole-image DIRECT call graph
(48,655 functions) finds no path: not from the drain loop's closure (34
functions), not from any of the 37 command Execute() bodies (one closes over
3,042 functions), not from the 4 destructors, and sub_82327BC8's 94 ancestors do
not include the drain loop. The positive control passes -- the same search
returns True from four known callers -- so the method works and the route must
run through one of the three bctrl in sub_82444EF0. Which one is unresolved.

CORRECTIONS TO EARLIER NOTES IN THIS ENTRY:
  - 'no call site tests thread identity' was over-claimed: it rests on decoding
    globals loaded before the branch, and there are bl/bctrl in that same window
    at every site, so a non-inlined IsInRenderingThread is not excluded by that
    method. The conclusion survives on the MEASUREMENT instead -- guest-7 was
    observed taking the enqueue arm -- so whatever screening exists did not stop
    it.
  - 'eight alternations in one run' is 4+4 in that run, ~30+30 corpus-wide.

WHERE THIS LEAVES THE FIX. The race is the title's, so there is nothing of ours
to stop doing. The reason it does not sink the console is not established and
matters: Xenon runs 6 hardware threads on 3 physical cores, so the game and
render threads may share a core and never truly overlap the RMW, while this port
runs them genuinely in parallel. If that is the explanation, this is a latent
title race that the console's topology hides and a PC port exposes -- and
serialising the commit becomes a defensible PORT decision rather than a bandaid.
Do not do it until the reason is established.

NEXT MEASUREMENT: a guest backtrace at the render thread's enqueue (the
instrument exists -- guest_backtrace.cpp) plus what host is doing at that
instant. That closes the indirect-dispatch gap, which is the only thing still
unknown.

### Note (2026-07-29)
THE ROUTE IS MEASURED. THE LAST UNKNOWN IS CLOSED.

A guest backtrace taken at the render thread's enqueue (the walk is bounded and
reports too-few-frames as a WALKER failure rather than as a short route, so a
silent success means something):

  0x82327d4c <- 0x82327b78 <- 0x8221d8f8 <- 0x8221b960 <- 0x8221b560
             <- 0x82444ea8 <- 0x82445004 <- 0x82445038 <- 0x8243ae00 <- 0x827a94ac

Resolved to functions, outermost last:

  sub_82444EF0   the drain loop
    --direct-->  sub_82444DB0     (bl 0x82444db0, returning to 0x82445004)
      --INDIRECT--> sub_8221B378
        --direct--> sub_8221B670
          --direct--> sub_8221D3A8
            --direct--> sub_82327B60
              --direct--> sub_82327BC8   enqueue FlushCommand at 0x82327d4c

CHECKED EACH EDGE rather than assuming: only sub_82444DB0 -> sub_8221B378 is
indirect; the other four are direct calls.

THIS CORRECTS AN EARLIER NOTE. The route was said to run through 'one of the
three bctrl in sub_82444EF0'. It does not: the drain loop reaches sub_82444DB0
by a DIRECT bl, and the single indirect hop is one level further down, inside
sub_82444DB0. The whole-image direct call graph found nothing because the break
was at the FIRST hop out of that function, not deep in a command's Execute --
which is also why the closure from the 37 Execute bodies was negative and the
positive control still passed. The method was sound; the inference about where
the gap lay was wrong.

WHAT IT MEANS: the rendering thread enqueues onto the ring it is draining, from
inside its own drain loop, via a callback. The single-producer invariant is
broken by the title's own control flow, not by anything this port schedules.

The fix question is unchanged and still open: why the console tolerates the
non-atomic commit under two producers. Serialising the commit remains a PORT
decision to be justified, not a bandaid to reach for -- see the note above.

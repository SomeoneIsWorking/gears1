---
id: 44
title: The guest stops progressing about 70 s in, on roughly two runs out of three
status: open
symptom: the audio pump stops at exactly 11250 callback invocations and VdSwap plateaus at 1860-1920 frames, while the process stays alive and the renderer keeps drawing; other runs sail past the same point
tags: hang,nondeterministic,guest,audio,blocker
created: 2026-07-28
updated: 2026-07-29
---

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

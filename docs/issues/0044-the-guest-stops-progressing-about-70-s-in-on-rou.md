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

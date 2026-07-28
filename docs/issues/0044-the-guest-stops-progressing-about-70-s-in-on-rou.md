---
id: 44
title: The guest stops progressing about 70 s in, on roughly two runs out of three
status: open
symptom: the audio pump stops at exactly 11250 callback invocations and VdSwap plateaus at 1860-1920 frames, while the process stays alive and the renderer keeps drawing; other runs sail past the same point
tags: hang,nondeterministic,guest,audio,blocker
created: 2026-07-28
updated: 2026-07-28
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

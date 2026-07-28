---
id: 43
title: The audio pump falls behind 187.5 Hz under a CPU-bound guest
status: open
symptom: over a 150 s run reaching gameplay the pump makes 16875 callback invocations where 187.5 Hz wants ~26250 -- about 120 Hz, so the title mixes slower than real time
tags: audio,performance,pump
created: 2026-07-28
updated: 2026-07-28
---

Found while verifying the XMA decoder, and deliberately NOT folded into that
result: it would have been convenient to blame the new decode and it is not the
cause.

MEASURED: decode costs 1.59 s across 71000 kicks over a ~140 s run (22 us mean,
2.4 ms worst). Removing 1.59 s from 140 s cannot turn 120 Hz into 187.5 Hz.

A control run with no audio pump at all reaches the same point in the game at
16.6 fps against 15.9 fps with the pump and decode, so the frame rate is
gameplay being CPU-bound rather than anything audio does.

CONSEQUENCE IF LEFT: the title produces audio frames slower than real time, so
a device consuming at 48 kHz will eventually starve. It has NOT been observed
starving yet -- a 100 s run played 10000+ frames with zero empty-device arrivals
-- because the shortfall accumulates slowly and the runs so far are short. A
longer session is the falsifier.

WHERE TO LOOK FIRST: the pump sleeps with sleep_until(next) and advances next by
a fixed period, so a callback that overruns its slot should be caught up by the
next iterations running back to back. That it does not catch up suggests either
the callback itself is slow under load (it runs guest code, including the
title's own mixing) or the host scheduler is not giving the pump thread time
against a guest saturating every core. Time the callback itself before assuming
either.

DO NOT "fix" this by lowering the pump rate to match what it achieves. The rate
is the console's driver contract; a title that mixes at 120 Hz is producing
audio for a clock that does not exist.

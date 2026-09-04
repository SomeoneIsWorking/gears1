---
id: 43
title: The audio pump falls behind 187.5 Hz under a CPU-bound guest
status: open
symptom: over a 150 s run reaching gameplay the pump makes 16875 callback invocations where 187.5 Hz wants ~26250 -- about 120 Hz, so the title mixes slower than real time
tags: audio,performance,pump
created: 2026-07-28
updated: 2026-08-04
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

MEASURED (2026-07-28, wall-vs-CPU + wait-site probes + /proc schedstat in the
pump, scratch/logs/pump_waitsites{3,4}.log): both of the above guesses are
wrong. The pump DOES run back to back (1875/1875 slots late in the heavy
phase), and it is neither slow nor starved:

- Heavy phase (17-18 fps): callback wall 14.0-16.9 ms mean; callback CPU
  93-111 us mean. The callback computes nothing for 99.4% of its duration.
- Not host scheduling: runqueue wait (schedstat) 5-62 ms TOTAL per ~31 s
  interval (~30 us per call), involuntary context switches ~150 per 1875
  calls. The thread is asleep by choice, ~5 voluntary switches per call.
- The named wait: KeWaitForMultipleObjects WaitAny on two dispatcher objects
  (the same wait catalog #40 identified) accounts for 5.8 s of every 6.0 s of
  callback wall time at 30 fps (KeWaitMultiple.any 5821 ms / 1875 calls);
  every other blocking primitive is instrumented and reads ~0.
- XMA decode is fully exonerated: 0 of 74000 kicks arrive on the pump thread.

So the pump is a CONSUMER waiting on the title's own audio pipeline: the
callback is "block until my mixer has a frame, then submit it". The producer
(the title's audio worker, a guest thread) delivers at ~187.5 Hz at the menu
and collapses to ~60 Hz (16.7 ms cadence) under load. The pump rate is an
EFFECT; the producer's cadence is the thing to explain.

NEXT MEASUREMENT: the 16.7 ms cadence is suspiciously exactly 60 Hz -- the
graphics ISR's rate -- which suggests the producer's WAKEUP is quantised by
something in this runtime (an event signalled from the 60 Hz vblank path, or a
coarse timer), not that it lacks CPU (8 cores, ~10 guest threads; at 30 fps it
keeps up while using the same machinery). Run a gameplay slice with
GEARS_LUCENT_DEBUG=wait and identify (a) which of the two WaitAny objects
satisfies the callback and (b) which thread Sets it, at what cadence. If the
signal chain runs through the vblank ISR or a coarse-grained runtime timer,
fixing THAT restores full-rate audio even in heavy scenes.

DO NOT "fix" this by lowering the pump rate to match what it achieves. The rate
is the console's driver contract; a title that mixes at 120 Hz is producing
audio for a clock that does not exist. Equally, do not raise the pump thread's
priority as a fix -- it is measured to not be starved.

### Note (2026-07-28)
ROOT-CAUSED, AND MY ORIGINAL FRAMING WAS WRONG. The pump is a CONSUMER.

The title's render callback is "block until my own mixer has a frame, then
submit it". So the pump's achieved rate is not the pump's property at all -- it
is the production rate of the title's audio pipeline. Measured on the pump
thread: wall 16.7 ms per callback in the heavy phase against 131 us of CPU, with
KeWaitMultiple.any accounting for 31316 ms of every 31400 ms. The pump is not
starved (runqueue 35 ms per 31 s interval) and its sleep_until loop does run
back to back (1875 of 1875 slots late). It is waiting, correctly, for a producer
that is slow.

WHO THE PRODUCER IS, measured rather than inferred: guest-4, the title's own
audio worker, signals the object the callback waits on -- 18884 times in one
run, and it is the ONLY setter. Its whole wait-channel footprint is that
handshake: 37770 KeWait and 18884 KeSetEvent, nothing else. It blocks on nothing
we implement. So the 16.7 ms is guest code running, not a wakeup being
quantised.

That falsifies the 60 Hz-wakeup hypothesis this entry previously carried. 16.7 ms
looked exactly like the vblank rate and was not: sampling the process during the
heavy phase found the worker inside XmaHwContext::Decode via the register-store
hook (its own thread, as designed), and only 3 of 20 threads running guest code
at all with the machine at 306% of 800%. Not starved, not quantised -- just slow.

THE CAUSE WAS THE BUILD. gears_ppc was compiled at -O0, with the comment "keeps
build times sane". Nobody had measured that. On this machine the entire
49k-function image builds in 98 s at -O1 and 106 s at -O2.

    opt   pump callback wall   invocations/run   gameplay fps
    -O0   16.7 ms              18750             16.3
    -O1    8.0 ms              22500             16.9
    -O2    6.7 ms              24375             17.8

So the producer is 2.5x faster for 106 seconds of build time that was already
being spent. Audio non-silence rises 81.8% -> 84.0%, and the XMA golden gate
still passes at correlation 1.000000, so this is a speed change and not a
behaviour change.

NOT FIXED, and this stays open: 6.7 ms per frame still overruns the 5.33 ms slot,
so the pump achieves roughly 150 Hz against the contract's 187.5. The remaining
gap is the title's mixer being genuinely expensive under guest execution. What has
changed is that it is now a performance problem with a measured size, not a
mystery.

ALSO WORTH FIXING SEPARATELY, found by the same instrumentation: the pump's
backlog is unbounded (14306 slots, ~76 s of audio debt, in a 200 s run). If the
guest ever recovers, the pump sprints and pushes faster-than-real-time audio at
the device. A real device never asks for the past; the backlog wants a clamp of
a few slots, or the device should pace the pump outright.

### Note (2026-08-04)
2026-08-04. MECHANISM NAMED, and it is not what the title of this entry says.

Traced with GEARS_LUCENT_DEBUG=wait over 577 consecutive audio frames. The
title's mixer is a TWO-THREAD PING-PONG, and our pump is one of the two threads:

  audio-pump: enters the guest callback -> KeSetEvent(0x82becc28) -> blocks in
              KeWaitForMultipleObjects on [0x82becc04 sync-event,
              0x82becc48 notification-event]
  guest-4:    wakes on 0x82becc28 -> mixes -> KeSetEvent(0x82becc04) -> blocks
              on 0x82becc28 again

577 signals from guest-4, 577 audio-pump waits: exactly 1:1, every slot. So one
audio frame costs a full round trip between two threads, and the audio frame
rate IS the round-trip rate. Measured on a gameplay walk: the callback's wall
time is 7.9-10.9 ms against a 5.33 ms slot while its CPU time is 0.09-0.21 ms --
it is not computing, it is waiting -- and the pump's backlog grows monotonically
(1432 -> 2669 -> 4644 slots over 30 s). The title therefore produces audio at
roughly half real time, which is what "crackling and wrong pitch" sounds like:
the device starves and the title's own audio timeline is stretched.

Rendering is NOT the cause. Control arm with the renderer effectively off
(GEARS_DRAW_FRAME_AT=99999999): the guest returns to 29.7 fps from 10.5, and the
pump is STILL 7.9-8.6 ms per slot with a growing backlog. Rendering makes it
worse (backlog grew ~1500 slots/10 s with it, ~900 without) but there is a
handoff problem underneath it.

RULED OUT as the dominant cost: the shared dispatcher condition variable. Every
signal used to wake every waiting guest thread; giving each waiter its own
condvar registered per object cut the pump thread's voluntary context switches
from 6629 to 2165 per 1875 slots and left the callback's wall time unchanged.
Landed anyway (it is strictly less work), but it is not the fix.

CAVEAT ON EVERY NUMBER ABOVE: this machine was at load average 28 from unrelated
builds throughout. Scheduler latency is part of what is being measured, and the
pump's own report says so (runqueue 0.8-1.6 s per 10 s interval, i.e. the pump
thread spent 8-16% of the time RUNNABLE but not running). These runs need
repeating on an idle machine before the size of the deficit is trusted -- but
the 1:1 ping-pong structure and the growing backlog are structural, not load.

NEXT: (1) re-measure idle; (2) the round trip is two condvar handoffs per 5.33 ms
slot -- the console paced this from the DAC IRQ with a real-time audio thread, so
raising the pump/guest-4 scheduling priority is the obvious first experiment;
(3) consider letting the pump run several slots ahead when the device queue is
draining, which is what the host device actually needs.

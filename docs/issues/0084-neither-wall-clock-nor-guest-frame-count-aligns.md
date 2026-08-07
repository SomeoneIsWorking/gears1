---
id: 84
title: Neither wall clock nor guest frame count aligns two runs: the title is not reproducible against itself
status: open
symptom: our own renderer, same input script indexed by guest frame, two runs: 98.9% identical pixels at frame 300, 25.9% at 600, 17.7% at 1200
tags: harness,oracle,determinism,lockstep,method
created: 2026-08-06
updated: 2026-08-07
---

## Why this was measured

`oracle_compare.sh` drives both sides on a WALL CLOCK and says, correctly, that a
pixel metric between its filmstrips is meaningless: two emulations run at
different speeds, so the same wall-clock offset is a different point in the game.
That looked like a fixable INDEXING problem, so both emulators were given the
guest's OWN frame counter as the index instead:

  * our runtime: `GEARS_INPUT_SCRIPT` accepts `f1500:A`, driven by VdSwap's
    counter through `SetGuestFrameSource`;
  * the oracle: `CommandProcessor::guest_swap_count()` (new -- `counter_` could
    not serve, GraphicsSystem also bumps it once per VBLANK, so it advances when
    the guest presents nothing), plus `--oracle_by_frame`, which drives input
    AND captures by that counter;
  * `tools/oracle_lockstep.sh` runs both from one schedule.

Both halves work. The oracle's captures land exactly on target ("captured guest
frame 300 (counter was 300)"), our frame-indexed steps fire at the right frame,
and cross-side alignment improves a lot: comparable screens at the same index
(9.2% identical pixels at frame 600) where wall-clock offsets showed unrelated
ones.

## THE CONTROL ARM KILLS IT

The harness runs OUR side TWICE, because "frame N is the same game moment" is a
claim about the TITLE, not about the index. Same binary, same script, same frame
indices, two runs:

    frame  300   98.96% identical pixels
    frame  600   25.90%
    frame  900   23.91%
    frame 1200   17.65%

**Our own renderer does not reproduce itself frame for frame**, and the
divergence grows with elapsed time. At frame 1200 a cross-side number measures
our own nondeterminism more than it measures the oracle, and no indexing scheme
repairs that: two sides cannot agree at a frame where one side disagrees with
itself.

## The likely reason, stated as a hypothesis

UE3 advances animation and physics on DELTA TIME, not on a frame count. Emulator
speed varies, so the same frame index is a different amount of GUEST TIME and the
simulation really is at a different point. Consistent with the shape -- 98.96% at
frame 300 before much has moved, degrading steadily after.

NOT ESTABLISHED: our own scheduling nondeterminism produces the same shape, and
the two have not been separated. Catalog #44 already records this title failing
to progress on roughly one run in three, so nondeterminism here is not news; what
is new is that it defeats frame indexing.

## What would actually work

Drive the GUEST'S CLOCK, not the schedule. If the time the guest reads advances
by a fixed step per presented frame, the simulation becomes a function of the
input schedule alone and both emulators reproduce -- against themselves and each
other. That means intercepting the title's time source on both sides. It is a
real change to each emulator, and it is the only thing that makes a per-pixel
cross-side comparison sound.

Until then, `tools/chroma_compare.py` is the honest instrument: compare
DISTRIBUTIONS that survive a moment mismatch, against a measured null band, and
quote no per-pixel number across the two sides.

## What is kept, and why this is not a dead end

The frame-indexed machinery stays. It is strictly better than wall-clock
indexing, it is what made this measurable at all, and it is a prerequisite for
the deterministic-clock work. The determinism control stays IN the harness so no
future run can quote a cross-side pixel number without seeing it first.

### Note (2026-08-06)
### Note (2026-08-06)
## "Drive the guest's clock" is necessary but NOT sufficient: a frame-stepped clock DEADLOCKS the title

This entry ends by naming the fix: "If the time the guest reads advances by a
fixed step per presented frame, the simulation becomes a function of the input
schedule alone and both emulators reproduce." That has now been BUILT on our
side and MEASURED, and the conclusion is that it does not work as stated. The
next session should not implement it again.

### What was built

`runtime/guest_clock.{h,cpp}`: one virtual clock, `GEARS_GUEST_CLOCK_STEP_NS=<n>`
(unset = real time, which is the default and is unchanged). Every clock the
guest can read is routed through it:

  * **mftb**, via a new `__ppc_set_time_base_source` hook in XenonRecomp's
    `ppc_context.h` -- generated code calls `__ppc_time_base()` at every mftb, so
    there is no other choke point;
  * **KeTimeStampBundle** (interrupt time, system time, tick count);
  * **KeQuerySystemTime**, whose wall-clock base is now frozen at process start
    so the absolute date does not vary within a comparison.

`AdvanceGuestClockFrame()` is called from `__imp__VdSwap`. The mode is reported
in both states -- a run that silently used the real clock and one that used a
fixed step must not be told apart by guesswork.

### And it deadlocks the title, measured

    real clock (control)            9 frames written in 100 s
    GEARS_GUEST_CLOCK_STEP_NS=16666667   0 frames written in 100 s

Not a slowdown: **zero presented frames, ever.** The guest is alive throughout --
17,000 audio frames submitted, 7.8 million kernel calls per second -- so this is
a spin, not a hang. The stall reporter names it:

    draw has made no progress for 8 s. The guest made 7807162 kernel calls in
    the last second (so something is running, just not this)
      guest-7 is running guest code, not in any kernel call
      gpu-isr is running guest code, not in any kernel call
      guest-1/2/5/6 have been in NtWait for ~7.7 s
    7 thread(s) blocked, 3 running guest code. A thread running while nothing
    progresses is a spin -- it is waiting on something it polls, not on us.

### The mechanism, and why it is a bootstrap problem rather than a bug

The clock advances only at VdSwap. Before the FIRST VdSwap the title is waiting
for time to pass -- in GUEST CODE, not in a kernel call, so it is an mftb spin
and not a timed wait anything could intercept. Time cannot advance until a frame
is presented; a frame cannot be presented until time advances. The first frame
is the deadlock.

That is not fixable by choosing a different step, and it is not specific to this
title's frame loop: any "advance only on event X" clock deadlocks on a spin that
must complete before X.

### What would actually work, and it is bigger than this entry says

A virtual-time scheduler: the clock advances when NO guest thread can make
progress without it. That needs the runtime to know every thread's blocked state
and every deadline, and -- because the blocker here is a guest-code spin rather
than a kernel wait -- it needs a deterministic notion of "this thread is
spinning" that does not depend on host timing. Detecting that cheaply is the
open problem, and it is emulator-scheduler work on BOTH sides, not a knob.

### What is kept

The plumbing, because it is correct and the default path is unchanged (31/31
tests pass; the control run writes the same 9 frames as before). The knob stays
off by default and says so. Also kept: a CMake drift guard, because
`ppc_context.h` is COPIED into the generated directory and the copy is what the
guest compiles against -- editing the submodule header alone does nothing, and
the symptom is a compile error in the runtime about a symbol that is plainly
present in the file you just edited. The guard fails the configure with the
refresh command; proved to fire on a deliberately stale copy and to pass on a
fresh one.

### Note (2026-08-06)
### Note (2026-08-06, same session) -- the discriminator, and it narrows the problem sharply

The note above concluded "a frame-stepped clock deadlocks" and left the real fix
as vague scheduler work. A control arm now separates the two halves of that, and
the answer is much better than the note implies.

`GEARS_GUEST_CLOCK_ON_VBLANK=1` steps the same virtual clock from the 60 Hz
VBLANK thread instead of from VdSwap. Vblank is paced by a host sleep and fires
whether or not the guest presents, so it cannot deadlock -- it is host-paced and
therefore NOT reproducible, and is a diagnostic only.

    real clock (control)                 9 frames in 100 s
    fixed step, advanced at VdSwap       0 frames  <- deadlock
    fixed step, advanced at VBLANK       9 frames  <- identical to the control

**So the guest is entirely happy running on a synthetic clock.** Every reader is
routed through it -- mftb, the KeTimeStampBundle, KeQuerySystemTime -- and the
title boots, loads, and presents exactly as it does on the real clock. Nothing
about virtualising the clock is the problem.

The problem is only the TRIGGER. Gating the clock on presents is circular: the
title spins in guest code waiting for time before its first present, so time
cannot advance until a frame is presented and a frame cannot be presented until
time advances.

### Which makes the next step concrete rather than open-ended

The trigger has to fire independently of presents, and deterministically. VBLANK
is the right event -- it is what the title waits on -- and the only reason the
control arm is not already the answer is that its vblanks are paced by a HOST
SLEEP.

Deliver vblank when the guest has finished consuming the previous one (fire the
next ISR once the previous returns) rather than on a 16.667 ms host sleep, and
step the clock per vblank. Then guest time is a function of the guest's own
execution and of the input schedule, with no host timing in it, and the run goes
as fast as the machine allows instead of at 60 Hz. That is the standard
free-running deterministic design and it is a bounded change to
`VblankThread` -- not the general scheduler work this entry previously implied.

The risk to watch is that "as fast as the guest consumes them" changes the
relative rate of the vblank ISR against the title's other threads, so the
determinism control (ours vs ours2 at frames 300/600/900/1200) is the gate, not
a boot that merely looks normal.

### Note (2026-08-07)
### Note (2026-08-07) -- the free-running vblank was built, and it FREEZES THE PICTURE

The note above proposed the concrete next step: "deliver vblank when the guest
has consumed the previous one rather than on a 16.667 ms host sleep, and step the
clock per vblank. Then guest time is a function of the guest's own execution."
That is now built (`GEARS_GUEST_CLOCK_TRIGGER=vblank-freerun`) and it does not
work. Recording it in full because it very nearly passed its own gate.

### It boots, and it looks reproducible

    frame 300, five independent runs        BIT-IDENTICAL, mean 160.8
    frame 3,300, two independent runs       bit-identical
    frame 7,800, two independent runs       BIT-IDENTICAL

against the real clock's 98.96% at 300, 25.90% at 600, 23.91% at 900, 17.65% at
1,200. On those numbers the mode is a total success.

### And the numbers are a match on a STILL IMAGE

The frames that reproduce perfectly are frozen. Within a single run:

    frame  300   0% identical to the run's last frame   mean 160.8
    frame 1200   0.6%                                   mean  20.4
    frame 1500   0.6%                                   mean  24.5
    frame 2700  99.91%                                  mean   9.7
    frame 3300 .. 10800   100.00% every one             mean   9.7

From about frame 2,700 the title presents the SAME IMAGE forever. The counter
keeps advancing to 10,800 -- it is presenting, so nothing looks stuck -- but
nothing moves.

**The control that catches it is the real clock**, and it is the only thing that
does. Consecutive frames of the real-clock arm on the identical walk are 21%,
22%, 34%, 27%, 27%, 32%, 28% identical to the run's last frame: a picture that
is alive. Every free-running arm sits at 100%.

So: a frozen picture is trivially reproducible, and the determinism control this
mode exists to pass is PASSED PERFECTLY BY A STILL IMAGE. Any determinism number
must be read together with "does the picture change at all". The mode now says
this in its own startup warning and in its header.

The likely mechanism, NOT confirmed: free-running vblanks arrive far faster than
60 Hz, so guest time advances at hundreds of times real rate and the title's
per-frame delta becomes absurd. A `std::this_thread::yield()` between vblanks
was tried first (the free-run loop re-takes g_interruptMutex with no gap and
starves everything else); it changed throughput and did not unfreeze anything.

### What is now known, and it pins the design problem exactly

  * The boot spin waits on TIME, not on vblank. Proved: the present-triggered arm
    had host-paced vblanks arriving at 60 Hz throughout and still deadlocked at
    0 frames. So vblanks alone do not release it.
  * The guest is happy on a synthetic clock -- the host-paced vblank-triggered
    arm boots and stays ALIVE (9 frames, same as the control). It is only
    unusable because a host sleep paces it.
  * Free-running the vblank makes guest time race, and the picture stops.

So guest time must advance BEFORE the first present (or boot deadlocks) and at a
fixed rate PER PRESENT afterwards (or the picture freezes). Those two conflict
only during boot, and that is the whole remaining problem. A rate-limited vblank
-- free-running until the first present, then pinned to a fixed vblank:present
ratio -- satisfies both, but its boot phase is host-paced and therefore seeds the
run nondeterministically, which may or may not matter and has not been measured.

### Also measured, and it is not new

Roughly two runs in five stall early (max guest frame 300) under every mode
tried. That is catalog #44, not this work.

### Note (2026-08-07)
### Note (2026-08-07) -- third design: pin vblanks to presents. Starves after 3 frames.

The note above stated the problem exactly: guest time must advance BEFORE the
first present (or boot deadlocks) and at a fixed rate PER PRESENT afterwards (or
the picture freezes), and said those conflict only during boot. So the obvious
design is to separate the phases: free-run vblanks until the first present, then
allow at most `presents + slack` of them. Built as
`GEARS_GUEST_CLOCK_TRIGGER=vblank-paced`. It does not work, and the reason kills
the whole family.

    vblank-paced, slack 2    0 presents past boot
    vblank-paced, slack 64   0 presents past boot

The gate now reports why instead of leaving it to be inferred from a dump count:

    vblank-paced is STARVED: 67 vblanks delivered since the first present,
    budget 67 (3 presents + 64 slack), and the guest has not presented again.

**Presents are not a proxy for time.** The title presents three times, then
loads -- and while loading it presents rarely and still expects time to pass.
Any budget expressed in presents starves there, and no constant slack fixes it
because the deficit grows without bound during a load. On hardware the same
title gets 60 vblanks a second regardless of whether it is presenting, and its
present rate floats underneath that (measured: ~27 fps against 60 Hz vblank in
normal play, far less while loading).

### The three failures, and the one thing they have in common

    present-triggered   boot deadlocks         the boot spin waits on TIME
    vblank-freerun      the picture freezes    guest time outruns the rendering
    vblank-paced        starves after 3 frames presents are not a proxy for time

Guest time has to advance independently of presents (or 1 and 3), but at a rate
that does not outrun the work actually being done (or 2). Presents, vblanks and
the host clock have each now been tried as the anchor and each fails one of
those two requirements.

### What is left, and it is a different kind of anchor

Derive vblank delivery from a deterministic measure of GUEST WORK rather than
from presents or from the host clock -- deliver a vblank every K guest kernel
calls, say, with K set so the rate is about 60 Hz under typical load. That
satisfies both requirements by construction: loading does work, so time advances
during a load; rendering does work, so time cannot outrun it.

The open question, and it should be measured before the design is built: the
kernel-call count is summed across all guest threads, and their interleaving is
still scheduled by the host, so the count at a given point is not obviously
reproducible. Cheap to check -- run twice and compare the kernel-call count at
each present -- and that measurement should come BEFORE any more emulator work.

### An instrument defect fixed on the way

"0 frames" was being read as "never presented" when the dump interval was 300
frames; the paced mode was in fact presenting 3 times and then starving. The
diagnosis was wrong for one round because of it. Progress is now measured with
`GEARS_DRAW_FRAME_REPORT_EVERY=10`, and the gate reports its own starvation.

### Note (2026-08-07)
### Note (2026-08-07) -- the fourth candidate is dead too, and the reason is structural

The note above named the next design (derive the clock from a deterministic
measure of GUEST WORK rather than from presents or the host clock) and said the
prerequisite measurement -- is guest work itself reproducible? -- should come
BEFORE building it. It has been made, and the answer is no.

`GEARS_WORK_TRACE=<path>` logs the guest's kernel-call count at every present.
Two runs, same binary, same input script, both under a fixed-step clock:

    present      run 1            run 2         difference
          1          1,006            1,006      0.00%
          2        165,119          164,352      0.46%
          3        193,547          208,850      7.91%
          5        297,935          367,338     23.29%
         10        454,228          535,529     17.90%
        100      2,598,721        2,706,966      4.17%
      1,000     64,598,380       72,536,681     12.29%
      5,000    351,719,228      393,047,733     11.75%
     20,000  1,547,925,251    1,585,164,696      2.41%

Of 22,209 presents common to both runs, the count matches at exactly **one** --
the first. By the third present the two runs differ by 8%, and they stay roughly
12% apart for the rest of the run.

### Why this is bigger than the fourth design

The kernel-call count is summed across all guest threads, and those threads run
on host threads that the OS schedules. How much work has happened by present N
therefore depends on host scheduling, and **any clock anchored to observed guest
activity inherits that**. This is not a property of kernel calls specifically;
it would be equally true of retired blocks, instructions, or any other aggregate.

So the conclusion is structural, and it applies to every design in this entry:

**A deterministic guest clock requires deterministic SCHEDULING of the guest's
threads.** As long as the title's threads are real host threads preempted by the
OS, no choice of clock anchor -- presents, vblanks, host time, or guest work --
makes two runs reach the same state at the same frame. Four anchors have now
been tried and each fails for its own reason, but this is the reason underneath
all of them.

That is a serialised or cooperatively-scheduled guest, on BOTH emulators. It is
a large piece of work and it should be a deliberate decision, not something a
harness session drifts into.

### What this means for the comparison the entry exists to enable

A per-pixel, frame-indexed cross-emulator comparison is BLOCKED on that
scheduling work and should not be attempted before it. The alternative, which
needs none of it, is to compare things that do not require frame-exact
alignment: the set of shaders and passes each side runs, per-pass draw counts,
the resolve structure, regional colour statistics, and content-aligned frame
matching (`tools/oracle_cache.py match`) instead of index-aligned. Those answer
"what does the game do differently" without requiring the two runs to be the
same run.

### Note (2026-08-07)
### Note (2026-08-07) -- the audio-pump control, which was the obvious objection

The claim above (guest work is not reproducible, so no clock anchored to it can
be) had one cheap alternative explanation: our audio pump submits at a fixed
187.5 Hz against REAL time, so it is a host-paced guest thread that would inject
nondeterminism into the kernel-call count on its own, regardless of scheduling in
general. Run with `GEARS_AUDIO_PUMP=0`:

    audio pump ON    identical at 1 of 22,209 presents, ~12% apart by present 1,000
    audio pump OFF   identical at 1 of 18,500 presents,  ~3% apart by present 1,000

So the audio pump is roughly three quarters of the MAGNITUDE and none of the
CAUSE. With it disabled the count still matches at exactly one present out of
eighteen thousand. The structural conclusion stands: it is the scheduling of the
guest's threads, not any one host-paced thread.

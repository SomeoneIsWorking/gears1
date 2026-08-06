---
id: 84
title: Neither wall clock nor guest frame count aligns two runs: the title is not reproducible against itself
status: open
symptom: our own renderer, same input script indexed by guest frame, two runs: 98.9% identical pixels at frame 300, 25.9% at 600, 17.7% at 1200
tags: harness,oracle,determinism,lockstep,method
created: 2026-08-06
updated: 2026-08-06
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

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

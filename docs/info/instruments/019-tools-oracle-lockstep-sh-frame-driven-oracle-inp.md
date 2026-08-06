---
id: I019
kind: instrument
status: trusted
created: 2026-08-06
---

## Instrument

tools/oracle_lockstep.sh frame-driven oracle input

## Validated by

CAUGHT INERT 2026-08-06 and fixed the same day. ParseInputScript stored schedule times as at_seconds*1000 (milliseconds), but under --oracle_by_frame the tick source is guest_swap_count(), so START@150 fired at frame 150000 -- never. Measured before: a 1200-frame run with the lockstep script's own input reported 0 button presses while capturing 3 of 3 frames, i.e. three title-screen frames with the emulator never touched. Fixed by scaling raw schedule units at use time (UnitScale(): 1000 on a wall clock, 1 under frame indexing; hold 120 ms vs 8 frames). Verified BOTH ways: frame mode now reports 13 presses at exactly the scripted frames (START at 150, A at 300, START+A at 420 for the 270-frame repeat), and wall-clock mode still fires START at 25017 ms, A at 30019 ms, LY+ at 45006 ms with its frames captured. EVERY cross-side number oracle_lockstep.sh has ever produced compared our walked runtime against an undriven oracle.

## Known failure modes

(none recorded yet)

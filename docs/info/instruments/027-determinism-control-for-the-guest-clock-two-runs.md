---
id: I027
kind: instrument
status: trusted
created: 2026-08-07
---

## Instrument

Determinism control for the guest clock (two runs of our runtime, same input script, compared frame for frame)

## Validated by

DISTRUST ANY NUMBER FROM IT THAT IS NOT PAIRED WITH A LIVENESS CHECK. Under GEARS_GUEST_CLOCK_TRIGGER=vblank-freerun it reported frames 300, 3300 and 7800 BIT-IDENTICAL across independent runs -- against 17.65% at frame 1200 on the real clock -- and every one of those was a match on a FROZEN PICTURE: from frame 2700 the title presents the same image forever while its counter advances to 10800. The liveness check that catches it is 'how identical is each frame to the run's LAST frame': the real-clock arm sits at 21-34% (alive), every free-run arm at 100% (frozen). Both classes have been run. Never quote a cross-run identity percentage without it.

## Known failure modes

(none recorded yet)

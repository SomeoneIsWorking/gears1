---
id: I047
kind: instrument
status: trusted
created: 2026-08-12
---

## Instrument

tools/pair_score.py drift gate: pairing quality priced in FRAMES OF DRIFT off the console's own self-correlation curve, replacing an absolute threshold that was unreachable by construction

## Validated by

The old 0.60 gate was calibrated on the positive control -- our frame.ppm against our own resolve of the SAME instant, ~0.94 -- which no cross-emulator pair can reach, since each side advances the guest by wall-clock delta and our capture is the frame AFTER the camera matches. Driven on the capture it previously FAILED at 0.5666: the console's own curve from the winning frame is +1 frame 0.7478, +2 0.6147, +3 0.1988, so the pair is 2.1 frames of drift and PASSES. Both classes are reachable -- a score above the +1 point is reported as off the top of the scale (equivalent_drift returns None) rather than clamped, which is the overreach that falsified C046. REFUSES when the dump has no successor frames rather than falling back to a threshold. LIMITATION, stated in the tool: a guest frame is not a fixed amount of game time, so the curve must be recomputed per capture and drift figures are not comparable across runs -- the console's gap-1 colour self-correlation was 0.297 in one capture and 0.7478 in another.

## Known failure modes

(none recorded yet)

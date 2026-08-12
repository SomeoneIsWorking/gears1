---
id: I040
kind: instrument
status: trusted
created: 2026-08-12
---

## Instrument

tools/camera_match.py

## Validated by

Run against both classes with different exit codes: the clip-watch run contains the console's viewpoint (closest 3.94, median 59.08, furthest 105.62, exit 0) and the clip-hunt run does not (closest 14.97, median 976.35, furthest 1189.06, exit 1). It refuses with exit 2 when the oracle dump carries no camera or when our log has no constant dump for the shader, rather than reporting a match. Finds which of our frames stood where the console stood, by joining on the VIEW-PROJECTION (c230..c233) -- guest data both emulators carry -- instead of on frame index, draw ordinal or content predicate, all three of which were tried and all three of which silently compared different moments. It prints the whole distribution, so 'no frame matched' carries its denominator. BLIND SPOT: a matched camera is not a matched frame -- the residual distance still shows in any viewpoint-sensitive quantity, and nothing outside the camera (animation pose, particle state) is checked.

## Known failure modes

(none recorded yet)

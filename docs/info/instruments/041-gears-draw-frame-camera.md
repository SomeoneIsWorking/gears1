---
id: I041
kind: instrument
status: trusted
created: 2026-08-12
---

## Instrument

GEARS_DRAW_FRAME_CAMERA

## Validated by

Run against both classes and two real constant layouts. Original c230..c233 positive/negative: pointed at the console's own constant dump it walked into range and captured; pointed at an edited 9999 camera it held 900 frames, captured NOTHING and reported both current and best distance. Generalised-base positive: scratch/camerapair_worldcam_positive_20260814 uses static-world VS cb3cec323318973e and its microcode-established c8..c11 view-projection; it held 524 frames, matched at rotation 0.0004/0.005 and relative translation 0.00037/0.013 (0.08 thresholds), and produced a provenance/UI-clean pair scoring 0.9458, better than the oracle's +1-frame self score 0.7844. Generalised-base negative: the later collision-diverged route found the same shader and finite matrices but never came closer than 32.54 thresholds, captured NOTHING and refused. The base is validated as decimal c0..c252 by camera_pair.sh and recorded in provenance. Holds every frame whose draws of GEARS_DRAW_FRAME_NEEDS' shader are outside separate rotation/relative-translation limits, reading the selected four rows from each draw's own register snapshot. The matched frame itself is captured; animation pose and particle state remain outside the camera gate.

## Known failure modes

Before 2026-08-14 the gate hardcoded c230..c233, so a later scene whose skinned camera shader was absent could not be paired even though its static-world shader carried the same view-projection at c8..c11. Fixed by an explicit, range-checked, provenance-recorded constant base. A shader's matrix layout must be established from its microcode; choosing a base because it gives a low distance would fit the instrument to the output.

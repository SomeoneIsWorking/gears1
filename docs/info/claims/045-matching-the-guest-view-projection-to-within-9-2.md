---
id: C045
kind: claim
status: holds
created: 2026-08-12
tags: oracle,comparison,pairing,camera
depends: runtime/vd_null_gpu.cpp, tools/camera_pair.sh
falsified_on: 2026-08-12
reconfirmed: 2026-08-12
verified_at: 2026-08-12 15:40:08
---

## Claim

Matching the guest view-projection to within 9.29 pairs the two emulators to a log-luminance correlation of 0.376, against 0.938 for a genuine match -- so the camera gate at threshold 10 is not sufficient for a pixelwise cross-console comparison.

## Evidence

scratch/camerapair, first run with every precondition satisfied: console dumped 60/60 window frames; camera taken from shader f3e9368c1bb68ecc at guest frame 793 and verified inside the window; provenance MATCH with one frozen camera digest; gate matched at 9.29 of a threshold of 10 after 191 frames held. 60 candidates scored; peak at f793 (the camera's own frame) at 0.3761, decaying monotonically 794=0.328, 795=0.292, 796=0.120. Positive control 0.9377.

## What would falsify it

a run at a tighter CAMERA_NEAR that scores materially above 0.376 -- that would mean threshold 10 was merely too loose rather than the view-projection being an insufficient key

## FALSIFIED 2026-08-12

WRONG, AND THE GATE IT WAS MEASURED AGAINST WAS CALIBRATED ON THE WRONG CONTROL. I concluded 'the camera gate at threshold 10 is not sufficient' from a colour score of 0.376 against a 0.60 gate. But 0.60 was set against a ZERO-MOTION control -- our frame.ppm against our own resolve of the SAME frame, 0.94 -- and no cross-side pair can ever be zero-motion, because the capture is by construction the frame AFTER the camera match. THE RIGHT YARDSTICK IS THE CONSOLE AGAINST ITSELF ONE FRAME APART, and it is 0.297 in colour (mean over 8 consecutive pairs from its own dumped window; 0.181 at gap 2, 0.088 at gap 3). So two frames from the SAME renderer, one frame apart, correlate at 0.30 -- and our camera-gated pair scores 0.376, BETTER than one frame of the console's own motion. The pairing was never what failed. A gate of 0.60 demanded a pairing tighter than one frame, which is unreachable at this scene's rate of change. AND THE DEPTH SCORE AGREES: the same pair scores 0.890 on depth against a console-self gap-1 depth figure of 0.621, so on both artefacts our pair beats the one-frame yardstick. WHAT REMAINS OPEN, and is now the real question: WHY is one frame worth so much? The oracle dumps ~18 resolves per frame at roughly 0.8 fps, and both emulators advance the guest by WALL-CLOCK delta -- so while dumping, each console frame covers on the order of a second of guest time. Consecutive dumped frames are therefore far apart in the GAME's clock, which is a property of the dumping and not of the scene. That is what caps the achievable correlation, and it is fixable at the source rather than by tuning a threshold.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.

## Re-confirmed 2026-08-12

REINSTATED: I falsified this too quickly and the argument I used has itself been refuted within the hour. C045 said matching the view-projection to within 9.29 under the old max-abs metric gives 0.376 and is not sufficient for a pixelwise comparison. I withdrew it on the grounds that 0.376 beats the console's own gap-1 self-correlation of 0.297, so the pairing must already be as good as it can get. THE NEXT RUN REFUTES THAT: with rotation constrained -- rotation 0.0013 against a 0.01 limit, translation 0.00057 of its own magnitude against 0.013, i.e. 0.13 thresholds -- the same comparison scores 0.6082 against console frame 790 and PASSES the 0.60 gate, with the profile falling away cleanly either side (791 at 0.465, 792 at 0.360, 793 at 0.150) and a positive control of 0.9371. So the old gate was leaving most of the available agreement on the table and was insufficient exactly as this claim said. Beating the gap-1 yardstick is not the same as being as well paired as possible, and I conflated the two.

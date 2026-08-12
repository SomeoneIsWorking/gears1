---
id: C045
kind: claim
status: holds
created: 2026-08-12
tags: oracle,comparison,pairing,camera
depends: runtime/vd_null_gpu.cpp, tools/camera_pair.sh
---

## Claim

Matching the guest view-projection to within 9.29 pairs the two emulators to a log-luminance correlation of 0.376, against 0.938 for a genuine match -- so the camera gate at threshold 10 is not sufficient for a pixelwise cross-console comparison.

## Evidence

scratch/camerapair, first run with every precondition satisfied: console dumped 60/60 window frames; camera taken from shader f3e9368c1bb68ecc at guest frame 793 and verified inside the window; provenance MATCH with one frozen camera digest; gate matched at 9.29 of a threshold of 10 after 191 frames held. 60 candidates scored; peak at f793 (the camera's own frame) at 0.3761, decaying monotonically 794=0.328, 795=0.292, 796=0.120. Positive control 0.9377.

## What would falsify it

a run at a tighter CAMERA_NEAR that scores materially above 0.376 -- that would mean threshold 10 was merely too loose rather than the view-projection being an insufficient key

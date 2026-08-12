---
id: C043
kind: claim
status: holds
created: 2026-08-12
tags: oracle,comparison,pairing,instrument
depends: tools/layer_capture.sh, tools/front_buffer_percentiles.py
---

## Claim

The content-based frame selector pairs the two emulators to a log-luminance correlation of only 0.49 at its best, against 0.94 for a genuine match -- so it cannot support a pixelwise cross-console comparison.

## Evidence

scratch/paircap (same-run, provenance-stamped, gpuguard-clean). Our front-buffer resolve scored against all 12 console front-buffer candidates in the window: peak 0.493 at f876, falling monotonically to 0.100 at f886, with the best-fitting shift growing steadily with temporal distance (0, 8, 16, 32, 40, 40 px) -- a moving camera. Positive control (our frame.ppm vs our own front-buffer resolve, same metric, same quantization) 0.939.

## What would falsify it

a content-selected pair that scores above 0.60, which would mean this run's camera motion was unusually fast rather than the selector being too coarse

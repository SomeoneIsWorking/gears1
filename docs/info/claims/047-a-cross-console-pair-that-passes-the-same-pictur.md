---
id: C047
kind: claim
status: holds
created: 2026-08-12
tags: oracle,comparison,pairing,camera
depends: runtime/vd_null_gpu.cpp, tools/camera_pair.sh
reconfirmed: 2026-08-12
verified_at: 2026-08-12 17:31:57
---

## Claim

A cross-console pair that PASSES the same-picture gate is achievable: constraining rotation and translation separately gives colour 0.6082 and depth 0.8900 against the console, where the orientation-blind metric gave 0.376.

## Evidence

scratch/camerapair_rot, one oracle run, our side camera-gated to a frozen copy of that run's constants, provenance MATCH. Gate matched at rotation 0.0013 (limit 0.01) and translation 0.00057 of magnitude (limit 0.013) = 0.13 thresholds, after 170 frames held. 40 candidates scored: f790 0.6082, f791 0.4654, f792 0.3598, f793 0.1498 -- a clean interior peak. Positive control 0.9371, gate 0.60. Yardsticks: the console against ITSELF one frame apart scores 0.297 colour / 0.621 depth, so this pair is temporally closer to f790 than f789 is.

## What would falsify it

a repeat run at the same thresholds that fails the gate, which would mean 0.6082 was luck in how closely this run's camera path passed the console's viewpoint rather than the metric working

## Re-confirmed 2026-08-12

PARTIALLY CHALLENGED AND HELD, WITH ITS LIMIT NOW MEASURED. A repeat paired run (scratch/camerapair_ps) matched the camera at 0.26 thresholds -- twice as loose as camerapair_rot's 0.13 -- and scored 0.5666, FAILING the 0.60 gate where the first scored 0.6082. This claim's own falsifier was 'a repeat run at the same thresholds that fails the gate'. The thresholds were NOT the same: the camera match was 2x worse and the score fell monotonically with it, which is what a working metric does rather than evidence the first result was luck. What it does establish is that 0.60 sits AT THE EDGE of what this scene allows: two runs straddle it, and whether a capture passes depends on how closely that run's camera path happens to approach the console's viewpoint. A pair should be scored, never assumed to pass because a previous one did.

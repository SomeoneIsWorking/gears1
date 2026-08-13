---
id: 101
title: first_divergence falsely named f7 by subtracting correlations of unrelated passes
status: resolved
symptom: first_divergence.py reports FIRST OBSERVABLE LOSS at f7 even though the equal-state final buffer passes and f7 is near its own temporal curve
tags: oracle,render,tooling,correlation,temporal-drift
created: 2026-08-13
updated: 2026-08-13
---

Root cause: the tool compared each pass to the previous pass by raw correlation, although velocity, masks, depth and colour have different content and temporal volatility. It also used a +1-frame yardstick for a pair that pair_score priced at 1.13 frames. Thus velocity 0.9731 to f7 0.7606 looked like a 0.2125 renderer loss.

Fix: --drift-frames loads the floor/ceiling console successors and interpolates the SAME pass self curve. A pass is now flagged only when its cross score falls at least --drop below that curve; raw cross-pass deltas remain display-only. The selftest requires a 0.01 deficit to be explained and a 0.35 deficit to fire.

Real discriminator: scratch/camerapair_short_ui_20260813 frame 901 at 1.13 frames. f7 self expectation 0.7723, native 0.7606, deficit 0.0117. Twelve decodable resolves all remain within 0.15; final buffer deficit 0.0001. I046 and the old f7 frontier are withdrawn; C053 records the corrected denominator and exclusions.

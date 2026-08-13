---
id: C053
kind: claim
status: holds
created: 2026-08-13
tags:
depends: tools/first_divergence.py, tools/pair_score.py
---

## Claim

At the equal-state f901 pair, no decodable resolved pass shows a renderer loss beyond its own drift-matched console self curve

## Evidence

pair_score prices scratch/camerapair_short_ui_20260813 at 1.13 frames. first_divergence.py --frame 901 --yardstick --drift-frames 1.13 compares 12 decodable passes: maximum deficit is scene colour 0.0677; f7 is 0.0117/0.0118; atlas 0.0037; front buffer 0.0001, all below 0.15. Depth and velocity score above their self curves. Structural console-only 1280x208 copies and three undecodable small f32 copies remain explicitly outside the denominator.

## What would falsify it

Any equal-state capture at a pair_score-priced drift where a decodable resolved pass falls at least 0.15 below its interpolated same-pass console self curve, or a correction to the decoder/metric that changes one of these deficits past 0.15.

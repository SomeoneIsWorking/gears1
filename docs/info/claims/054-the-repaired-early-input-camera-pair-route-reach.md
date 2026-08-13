---
id: C054
kind: claim
status: falsified
created: 2026-08-13
tags: oracle,render,comparison,character
depends: tools/camera_pair.sh, tools/first_divergence.py, tools/pair_score.py
falsified_on: 2026-08-14
---

## Claim

The repaired early-input camera-pair route reaches a materially different character-heavy gameplay view, and every decodable resolved pass matches its own drift-adjusted oracle curve within 0.15

## Evidence

scratch/camerapair_character_20260813 from camera_pair.sh with f110 START/f260 A: both input arms fired; provenance pair camerapair-20260813T204121Z-630226 and camera digest 7c294659e4f26458 match; oracle camera f838; native camera match 0.86 thresholds; pair_score 0.4902 = 2.1 oracle frames of measured drift. first_divergence.py at f838/drift 2.1 compared 12 decodable resolves; maximum positive deficit 0.0079 (depth atlas), final buffer 0.0038, none >=0.15. The view contains multiple characters behind a foreground grate, materially different from C053s static doorway.

## What would falsify it

Any repeat of the 110/260 route that fails input/provenance/camera/drift gates, or any corrected decoder/metric yielding a >=0.15 drift-matched deficit in this pair.

## FALSIFIED 2026-08-14

Directly rendered native frame.ppm visibly contains the NO STORAGE DEVICE modal in both scratch/camerapair_character_20260813 and scratch/camerapair_turn_strict_20260813, while the oracle f900/f838 image does not. The input/provenance/camera/drift gates therefore accepted unequal UI state; their pass scores cannot establish renderer fidelity.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.

---
id: C055
kind: claim
status: falsified
created: 2026-08-14
tags: oracle,render,pairing,ui
depends: tools/camera_pair.sh, tools/ui_state_check.py, tools/first_divergence.py, extern/xenia/src/xenia/base/threading_posix.cc
falsified_on: 2026-08-14
---

## Claim

The corrected f110 START/f600 A camera-pair route yields an overlay-free paired gameplay frame, and all decodable resolves stay within 0.15 of their drift-matched oracle curves

## Evidence

scratch/camerapair_ui_repaired_20260814: both input arms fired; native log records automatic storage selection; direct frame render has no modal; ui_state_check scans 863 draw rows and finds the one clean UI shader occurrence; provenance/camera digest bbe1b005d8c31e1d matches; strict camera gate 0.86 thresholds; pair_score selects oracle f901 at 0.7285 = 1.6 console frames. first_divergence compares 12 decodable resolves: maximum deficit 0.0986, final 0.0049, none >=0.15.

## What would falsify it

Any repeat of the default route failing startup/input/UI/provenance/camera/drift gates, or a corrected decoder/metric yielding >=0.15 deficit on this pair.

## FALSIFIED 2026-08-14

Withdrawn because the claim combines valid input/UI/camera capture evidence with a renderer-parity conclusion derived from the retired score-based first_divergence workflow.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.

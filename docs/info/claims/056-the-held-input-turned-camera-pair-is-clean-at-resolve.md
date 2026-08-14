---
id: C056
kind: claim
status: falsified
created: 2026-08-14
tags: oracle,render,pairing,input,ui
depends: tools/camera_pair.sh, tools/menu_walk.sh, tools/ui_state_check.py, tools/first_divergence.py, extern/xenia/src/xenia/base/threading_posix.cc
falsified_on: 2026-08-14
---

## Claim

The held-input route produces a clean, turned gameplay pair, and none of its 12 decodable resolve boundaries falls 0.15 below its own drift-matched oracle curve.

## Evidence

scratch/camerapair_turn_clean_final_20260814 used `90:START~120 600:A 700:RX+ 780:RX0`. The rebuilt oracle captured 8/8 frames and selected f1081. Both roles carry pair id camerapair-20260813T214112Z-750500 and camera digest e21e25aa7d98cd5e. Native logged exactly one automatic storage selection; ui_state_check scanned 1658 draws and accepted measured clean class 2. The strict camera gate measured rotation 0.0030 (limit 0.005) and relative translation 0.00287 (limit 0.013), 0.60 thresholds. pair_score measured 0.9417 = 2.0 console frames. first_divergence at drift 2.0 compared 12 decodable resolves; the largest positive deficit was 0.0022 and none reached 0.15. It separately reported two console-only 1280x208 resolves, two too-sparse passes and three undecoded 352x182 f32 resolves.

## What would falsify it

A repeat of this route failing startup/input/UI/provenance/camera/drift gates, or a corrected decoder/metric yielding a >=0.15 deficit on this preserved pair.

## FALSIFIED 2026-08-14

Withdrawn because its no-resolve-loss conclusion comes from the retired aggregate/per-pass correlation scorers rather than an exact first divergent draw.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.

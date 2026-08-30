---
id: 160
title: Shared native-pass registry contains Gears 1 shader identities
status: resolved
symptom: runtime/native_pass.cpp mixes title-neutral enable/find/report behavior with six exact Gears 1 shader hashes and the scene-composite binding contract, so another title cannot compose the shared renderer without inheriting Gears 1 pass policy.
state_items: S003
tags: architecture,title-boundary,native-pass,shader,gearsue3
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

The first native-pass experiment put the exact Gears 1 roster beside the generic lookup machinery because only one title existed. The renderer seam is shared engine behavior; shader identities, pass names, evidence, and exact binding interfaces are revision-adapter data.

## Required resolution

Keep one title-neutral enable/find/report implementation that consumes one exact roster supplied by the linked title adapter. Move all Gears 1 shader hashes and scene-composite interface construction under runtime/titles/gears1, preserve declaration-only refusal and native vertex/pixel lookup behavior through the shipping implementation, and link the same exact adapter into both the product and the Gears 1 frame replay tool.

### Note (2026-08-30)
Resolved: runtime/native_pass.* now owns only title-neutral enable/find/report behavior and requires one ExactTitleRoster provider. runtime/titles/gears1/native_pass.cpp owns all six measured shader hashes, pass names/evidence, exact scene-composite interfaces, and module selection. The first direct split exposed renderer tests that linked gears_draw without an exact provider; the root composition fix keeps gears_draw neutral and adds gears1_native_pass plus the gears1_draw interface for every final Gears 1 product, frame-replay, and renderer-test consumer. Focused disabled/enabled controls prove exact-roster delegation, implemented pixel/vertex lookup, declaration-only refusal, and zero-hash refusal. The final Clang build, all 91 non-quality tests, the 617.44-second cpp_quality gate, and a 420-frame headless run pass.

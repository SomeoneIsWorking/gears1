---
id: C016
kind: claim
status: falsified
created: 2026-08-06
tags: render,gameplay-scene,catalog-77
depends: runtime/frame_content.cpp, runtime/gpu_draw_xlate.cpp
falsified_on: 2026-08-14
---

## Claim

Four of the fifteen frame captures in the tree submit a skinned character mesh (bright, black, play_v2, prison_turn); eleven do not, including the 744-draw gameplay frames act1, courtyard, walk_gameplay and walk_v3

## Evidence

tools/skinned_frames.sh over scratch/frames/*.gfr, 2026-08-06: 4 FOUND / 11 NONE. The detector is validated against both classes and self-tests (instrument I016). This is a claim about what each frame SUBMITS, not about what is visible.

## What would falsify it

a character drawn without a GPU bone palette (CPU-skinned or morph-target) would be invisible to this detector, so a capture shown by other means to contain a character while scanning NONE falsifies the count; the numbers are also specific to the 15 captures present on 2026-08-06

## FALSIFIED 2026-08-14

The capture corpus named by the claim has grown from 15 to 16, so its explicit count expired. The repaired full-corpus tools/skinned_frames.sh scan now examines all 16 and reports 5 FOUND / 11 NONE, with character_auto.gfr the fifth positive. Issue #105 separately records and fixes a current reporting-loop control-flow defect; it does not establish that the historical 2026-08-06 count was produced by the same broken revision.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.

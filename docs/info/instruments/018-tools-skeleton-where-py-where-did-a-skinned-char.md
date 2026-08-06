---
id: I018
kind: instrument
status: trusted
created: 2026-08-06
---

## Instrument

tools/skeleton_where.py (where did a skinned character's skeleton land?)

## Validated by

Run against BOTH classes in one command, and it will not report without doing so: the calibration draw (bright.gfr 460, which the GPU rasterised into 1431 primitives) comes back 44 of 45 joints on screen with an ndc spread of 1.26 x 1.66 -- a character filling the frame -- while character_auto.gfr 319, killed at clip, comes back 43 of 44 joints behind the camera. --selftest checks the arithmetic on synthetic constants with a known answer plus two negatives (an unknown shader hash must be REFUSED, an empty log must parse to nothing). TWO FAILURE MODES ALREADY CAUGHT AND FIXED, both by the calibration arm rather than by inspection: unused all-zero palette slots were being counted as joints at the world origin (9 phantom on-screen joints), and the first gate asked only for one on-screen joint, which a genuinely wrong layout passed with 1 of 45. BLIND SPOTS, printed: it knows ONE shader's constant layout (vs 0x15cbc482459fe5b7) and refuses every other rather than guessing; and it transforms the SKELETON, not the mesh, so a character straddling the frustum edge is not decidable this way.

## Known failure modes

(none recorded yet)

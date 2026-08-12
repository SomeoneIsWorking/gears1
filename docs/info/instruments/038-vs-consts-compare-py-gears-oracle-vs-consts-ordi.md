---
id: I038
kind: instrument
status: trusted
created: 2026-08-12
---

## Instrument

vs_consts_compare.py + GEARS_ORACLE_VS_CONSTS_ORDINAL

## Validated by

Run against both classes: 0 of 256 constants differ for a dump compared against itself, and 209 of 256 differ between two frames' dumps of the same draw ordinal, so it can print both answers. It also refuses (exit 1) rather than guessing when the log holds more than one dump of the named draw. Joins the two emulators' vertex float constants for ONE named draw ordinal, which is the only way to compare a per-bind input for a shader bound six times a frame. Two traps it refuses rather than papers over: our log holds one dump per FRAME and they disagree (six dumps of draw 294 differing in the local transform and in the bone-stride constant c[4] -- 3 in one frame, 4 in the other five), and the oracle's constant buffer is DENSE-PACKED so a slot index is not a guest register number (it walks the packer's own bitmap to print guest indices). BLIND SPOT: float constants only, and only meaningful for a moment both sides selected with the SAME content predicate.

## Known failure modes

(none recorded yet)

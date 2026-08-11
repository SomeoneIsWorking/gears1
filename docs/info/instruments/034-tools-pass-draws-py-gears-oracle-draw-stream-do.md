---
id: I034
kind: instrument
status: trusted
created: 2026-08-11
---

## Instrument

tools/pass_draws.py (+ GEARS_ORACLE_DRAW_STREAM): do the two emulators issue the same draws, with the same state?

## Validated by

Six self-test classes, each with the negative it could fail on: an identical pair that must report no difference WITH its denominators; a one-draw difference and both one-sided pairs that must all be named; an exactly-2x pair that must be separated as the predicated-tiling collapse while a genuine 4-vs-3 beside it is still reported; two draws of the SAME shader pair differing only in depth control, which must read as one pair each side rather than as agreement (if the key collapsed the state fields the whole capability would be silently absent); a wide table and a wide stream round-tripped through the REAL readers; and a narrow table against a wide stream that must be REFUSED.

On real data (scratch/widecheck, our side, current build): the reader sees the table as wide, 125 distinct keys over 865 draws against 118 narrow pairs for the same frame -- so the state fields add resolution without fragmenting the key. The runtime's raw columns agree with its own decoded ones (depth_control 0x8777 -> stencil_on 1, func 7).

WHAT IT PRODUCED BEFORE IT WAS TRUSTWORTHY, both now impossible: "ours 73, theirs 2" for one vertex shader, which was our table recording the pixel shader the guest BOUND for a depth-only draw where Xenia records zero for the null shader it binds (truth: 71 against 69); and every 2x row read as a difference, which was 70 of 74 rows.

WHAT IT CANNOT SEE, said in its own output: it counts draws and their state, NOT where they landed -- a draw with the wrong transform counts the same. It compares one frame per side, chosen by the caller.

## Known failure modes

(none recorded yet)

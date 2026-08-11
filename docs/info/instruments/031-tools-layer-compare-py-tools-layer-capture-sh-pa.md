---
id: I031
kind: instrument
status: DISTRUSTED
created: 2026-08-07
distrusted_on: 2026-08-11
---

## Instrument

tools/layer_compare.py + tools/layer_capture.sh (pair every RESOLVE of the first gameplay frame across our renderer and the console, per pass)

## Validated by

--selftest feeds the real untiler and the real join a pair it MUST call equal (an image tiled with the scalar Xenos swizzle and untiled again) and a pair it MUST call different (the same image rolled 7 px). Both fire; a tool that always said 'match' or always said 'DIFFER' fails one of them.

It has already been caught lying once and fixed: the first paired run decoded THREE passes whose guest destination format is k_16_16_16_16_FLOAT (8 bytes/px) as k_8_8_8_8, and reported plausible per-pass differences for them. The destination format is now part of the dump filename on both sides and any format other than k_8_8_8_8 is REFUSED per pass, counted, and printed in the same table -- never read as 8888.

Blind spot it states itself: it compares resolve DESTINATIONS, so a pass consumed without a resolve does not appear at all.

## Known failure modes

(none recorded yet)

## DISTRUSTED 2026-08-11

It reported the frame's DEPTH copies as passes only the console executes, when our renderer had been executing them all along -- the per-resolve snapshot was taken only on the colour branch, so a depth copy that ran wrote no file, and a pass missing from the output is indistinguishable there from a pass the renderer skipped. It also keyed our depth passes srcDFFFFFFFF/f6 against the console's srcD000/f23, so they could not have paired even with the files present. Fixed and re-validated as I032; every depth conclusion drawn from I031 (catalog #90, claim C026) is withdrawn.

> Every result this instrument produced is suspect until it is re-validated.

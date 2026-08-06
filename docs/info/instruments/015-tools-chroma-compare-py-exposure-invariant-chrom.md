---
id: I015
kind: instrument
status: trusted
created: 2026-08-06
---

## Instrument

tools/chroma_compare.py (exposure-invariant chromaticity comparison of our frame against the Xenia oracle, with a measured moment-mismatch null band)

## Validated by

Run against BOTH classes, not only the expected one. --selftest passes 7 cases: identity on a frame against itself (d=0); R<->B detected when present; a 0.30x-exposure copy STILL reporting identity (the property everything rests on -- without it the tool would be measuring catalog #62's brightness defect and calling it colour); dim AND swapped reporting R<->B; a same-palette different-moment pair landing ~1500x closer than an exchange; an all-black frame REFUSING rather than matching everything; an empty directory REFUSING rather than reporting no difference. On real data it declined to declare -- 'SUGGESTIVE, NOT SETTLED', because the winning permutation's worst pair (0.0184) exceeded the measured null band (0.0138). LIMITS: compares DISTRIBUTIONS not pixels (same palette rearranged scores identically); says nothing about exposure, geometry or a missing pass, by construction; sees an EXCHANGE of channels, never a per-channel SCALE; near-black px (R+G+B<24) excluded, surviving fraction printed; comparing a gameplay frame against a menu frame is meaningless and must be filtered by hand.

## Known failure modes

(none recorded yet)

---
id: I012
kind: instrument
status: trusted
created: 2026-08-05
---

## Instrument

tools/frame_stats.py

## Validated by

Ships --selftest, wired to cases whose answers are known and run before use: a synthetic half-red frame must report R/G 0.502 (it does); an all-black frame must report mean G 0.0 and be named as black rather than given a ratio (it does); the R/B-swap detector must fire on a swapped pair AND stay silent on an unrelated pair (both); and the PNG reader must produce bytes identical to the PPM reader on the same content, or comparing our PPM against a reference PNG would be comparing two decoders (it does). Also validated on real data: it reproduces catalog #62's independently-derived R/G 0.7684 on courtyard.gfr, and refuses with exit 1 on a missing file.

## Known failure modes

(none recorded yet)

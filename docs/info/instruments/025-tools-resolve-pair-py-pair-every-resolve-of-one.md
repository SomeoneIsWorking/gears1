---
id: I025
kind: instrument
status: trusted
created: 2026-08-06
---

## Instrument

tools/resolve_pair.py (pair every resolve of one captured frame across our renderer and the oracle)

## Validated by

Refusals exercised, all three fire with exit 1 and no table: a missing capture file, a log containing no after-resolve probe lines, and an oracle log whose resolve count disagrees with the capture's copy-draw count (a truncated --present resolve:N trace). Positive: on bright.gfr it pairs all 18 copy draws and separates them into 4 agree / 8 oracle-empty (#79 defect) / 3 depth blind spot / 1 not executed / 1 WE HAVE MORE / 1 ORACLE HAS MORE, i.e. it produces both agreement and disagreement rather than one uniform verdict. PAIRS BY DRAW INDEX, never by position -- our renderer executes 14 of 18, so position-pairing would compare unrelated buffers. The two percentages are DIFFERENT metrics (oracle: non-zero bytes over the tiled destination; ours: non-zero components over the untiled RGB image), so only gross disagreement is meaningful.

## Known failure modes

(none recorded yet)

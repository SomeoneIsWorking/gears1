---
id: I050
kind: instrument
status: trusted
created: 2026-08-13
---

## Instrument

tools/draw_interval_ledger.py + ownership-extended GEARS_ORACLE_DRAW_ORDER

## Validated by

Shipping ctest selftest feeds an exact aligned draw, the same shader/state/geometry with a different EDRAM colour owner, and an oracle-only inserted draw; all three answers fire. A real old 11-column oracle corpus is also refused after scanning 59 interval draws because it lacks ownership rather than being reported as a match.

## Known failure modes

(none recorded yet)

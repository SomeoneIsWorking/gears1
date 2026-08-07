---
id: I030
kind: instrument
status: DISTRUSTED
created: 2026-08-07
distrusted_on: 2026-08-07
---

## Instrument

Luminance-threshold bounding box over a presented frame (ad-hoc, used to file catalog #86) -- DISTRUSTED ON ARRIVAL

## Validated by

NOT VALIDATED, and recorded here so the next session does not reach for it. It measures the extent of the LIT SUBJECT and was read as the extent of the RENDERED IMAGE. On our Act 1 frame it reported content spanning 63% of the width with black borders; gamma-boosting the same frame by 0.28 shows those borders are fully rendered geometry -- the walls, ceiling and floor of a dark room -- with only 33% of the border pixels exactly zero and 63% sitting in 0.002..0.02, just under the 0.02 threshold the measurement used. The bright rectangle was a doorway. Compounded by then comparing that number against an oracle frame at a different camera position. If a bounding box is needed at all it must be paired with a gamma-boosted look at what it called empty, and it must never be compared across two frames not known to be the same shot.

## Known failure modes

(none recorded yet)

## DISTRUSTED 2026-08-07

Reports the lit subject's extent, not the rendered extent; 63% of what it called empty was rendered geometry just under its threshold. Was also compared across two frames at different camera positions. Filed catalog #86, now withdrawn.

> Every result this instrument produced is suspect until it is re-validated.

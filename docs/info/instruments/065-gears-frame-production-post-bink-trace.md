---
id: I065
kind: instrument
status: trusted
created: 2026-08-29
---

## Instrument

`GEARS_FRAME_PRODUCTION_TRACE=1` post-Bink render-ring and semantic-present trace

## Validated by

The focused timing test proves that present-boundary events can produce a report
after scheduler events stop. A 45-second headless no-render Gears 1 run produced
both answers: at approximately frame 571 it recorded 7,971 render-ring
reservations and 571 semantic present boundaries at 29.9/s; by approximately
frame 1,142 it recorded 81,643 reservations while the startup producer counters
remained at 993 dispatches and 429 blocked calls. The semantic present boundary
is emitted once per `VdSwap` on the live path.

## Known failure modes

The no-render arm still parses guest PM4 and accumulates draw commands, so it is
not a native-engine execution test and cannot identify the final host-renderer
cost. Ring reservations also do not name the gameplay wait or producer that
limits presents. The instrument measures a boundary; it does not authorize a
60 Hz timing override.

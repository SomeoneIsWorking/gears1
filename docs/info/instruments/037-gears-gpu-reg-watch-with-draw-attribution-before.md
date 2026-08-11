---
id: I037
kind: instrument
status: DISTRUSTED
created: 2026-08-12
distrusted_on: 2026-08-12
---

## Instrument

GEARS_GPU_REG_WATCH with draw attribution ('before draw N of this frame'), joined as a running value to say which constant a given draw saw

## Validated by

DISTRUSTED ON ARRIVAL, deliberately. It contradicts GEARS_DRAW_VS_CONSTS_VS on the same question: over six draws in two frames the constants dump reports c4 = 4 on every draw that rasterised and 3 on every one that clipped, while this watch's running-value join reports 3 on draws that survive (441, 444, 446) and 2 on one that clips (442). Both cannot be right. This one is the more indirect of the two -- it reads raw writes to a register shared by every shader, fired 1,239 times on that register inside a single frame, and reconstructs 'the value in effect at draw N' from an ordering between a register file, a snapshot and an upload, whereas the constants dump prints what the draw's own uniform data contained. A first version of the join was also wrong in a way that produced plausible numbers: matching the draw ordinal EXACTLY, so a draw with no write in its own window silently picked up a value from a different frame. Do not quote either instrument on constant values until they are made to print the same quantity for the same draw and one of them moves.

## Known failure modes

(none recorded yet)

## DISTRUSTED 2026-08-12

Contradicts I035's constants dump on c4 for the same draws; the more indirect of the two, and its join was already wrong once in a way that produced plausible numbers.

> Every result this instrument produced is suspect until it is re-validated.

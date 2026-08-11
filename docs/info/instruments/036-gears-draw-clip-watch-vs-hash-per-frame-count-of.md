---
id: I036
kind: instrument
status: trusted
created: 2026-08-12
---

## Instrument

GEARS_DRAW_CLIP_WATCH=<vs hash>: per-frame count of draws of that vertex shader that assembled primitives and lost EVERY one to clipping, holding the diag table from the frame it fires on

## Validated by

Both arms observed on real frames. Positive: it fires and holds -- 'CLIP WATCH f3e9368c1bb68ecc: 9 draw(s) of that vertex shader this frame, 1 of them assembled primitives and lost EVERY one to clipping', and the held table shows draw 457 with ia_prims 3310 and prims_after_clip 0 while its neighbours keep 501/736/526. Negative: on frames that do not contain the failure it prints the same line with 0 and the denominator ('4 draw(s) ... 0 of them'), so 'not this frame' is distinguishable from 'the watch is not running'; with no GEARS_DRAW_DIAG it says the statistics are missing and refuses to run rather than reporting zero. The first version was WRONG and the failure mode is worth keeping: the hold latch was a member of DrawStats, which is rebuilt every frame, so it never persisted and the table on disk was always a later non-firing frame -- caught because the held table contained no zero-after-clip row while the log said one had fired.

## Known failure modes

(none recorded yet)

---
id: I044
kind: instrument
status: trusted
created: 2026-08-12
---

## Instrument

GEARS_DRAW_FRAME_CAMERA camera gate (runtime/vd_null_gpu.cpp) + tools/camera_match.py

## Validated by

Key checked STATICALLY from the microcode rather than assumed: vs f3e9368c1bb68ecc references exactly c0..c4, c230..c233 and c255, and exports position as 'mad oPos, r2.wwww, c233, r1.zxyw', so c230..c233 is the view-projection the gate compares. The parser REFUSES and says so when the file lacks all four rows ('N of the 4 view-projection rows c230..c233. NO camera gate is running'), and the negative path reports the distance achieved and the best seen so far, so 'the camera never got there' and 'no draw of that shader ran' are distinguishable. A 2026-08-12 result that looked like the gate failing (0.07 correlation against the console) was traced to comparing across TWO DIFFERENT ORACLE RUNS, not to the gate -- see C042.

## Known failure modes

(none recorded yet)

---
id: I029
kind: instrument
status: trusted
created: 2026-08-07
---

## Instrument

GEARS_DRAW_STREAM_RAW (runtime/gpu_draw.cpp): the (vs,ps) pairs the GUEST PROGRAMMED, counted above every drop site

## Validated by

Validated against the arm it exists to distinguish itself from. Run side by side with GEARS_DRAW_STREAM (which records from `prepared`, below ten Skip/continue sites) over 12,257 frames: the raw stream reports 278 pairs against prepared's 279, 0 pairs programmed and never prepared, and exactly 2 pairs partially dropped -- both at exactly HALF their programmed count (16,992 -> 8,496 and 32 -> 16), which is the predicted signature of the predicated-tile collapse (claim C008) folding two tiles into one, not a bug. So it can report a drop, it did report the drops that exist, and it separates them from the zero case. It also refuses loudly rather than writing an empty file when its path will not open, because 'the guest programmed nothing' and 'nothing was recorded' are the two readings this project keeps confusing.

## Known failure modes

(none recorded yet)

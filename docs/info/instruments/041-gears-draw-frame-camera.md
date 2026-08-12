---
id: I041
kind: instrument
status: trusted
created: 2026-08-12
---

## Instrument

GEARS_DRAW_FRAME_CAMERA

## Validated by

Run against both classes. POSITIVE: pointed at the console's own constant dump it held 99 frames, reporting the distance as it walked in from 1157.89 to 590.67, matched at 3.77 and captured 18 files. NEGATIVE: pointed at a camera edited to (9999,9999,9999,9999) it held 900 frames, never came closer than 10,199, captured NOTHING and exited 4 -- and said so every 60 frames with both this frame's distance and the closest any frame had come. Holds every frame whose draws of GEARS_DRAW_FRAME_NEEDS' shader are further than a threshold from the console's view-projection, read out of each draw's own register-file snapshot. BLIND SPOT: the capture is the frame AFTER the match, so the viewpoint moves a little further and the residual shows in anything viewpoint-sensitive; the gate says so rather than pretending to be exact. Nothing outside the camera is matched -- animation pose and particle state can still differ.

## Known failure modes

(none recorded yet)

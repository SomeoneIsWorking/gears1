---
id: I042
kind: instrument
status: trusted
created: 2026-08-12
---

## Instrument

tools/img_extent.py (threshold-SWEEP bounding box + pixelwise compare)

## Validated by

Run against BOTH classes from the same real frame (--selftest): a synthetic inset built by zeroing the outer thirds of scratch/camgate/match/frame.ppm holds x 426..853 at every threshold from 0.002 to 0.100, while the unmodified frame moves from x 0..1279 at 0.002 to x 254..1013 at 0.100. So it reports an inset when there is one and does not when there is not. It exists because a SINGLE-threshold bounding box is what produced catalog #86 twice; it prints six thresholds plus a per-column luminance profile and refuses to answer with one box.

## Known failure modes

(none recorded yet)

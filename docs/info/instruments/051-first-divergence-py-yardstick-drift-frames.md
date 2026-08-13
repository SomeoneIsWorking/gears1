---
id: I051
kind: instrument
status: trusted
created: 2026-08-13
---

## Instrument

first_divergence.py --yardstick --drift-frames

## Validated by

Selftest interpolates a 1.25-frame self curve and requires a 0.01 deficit to pass and a 0.35 deficit to fail. On scratch/camerapair_short_ui_20260813 at its pair_score-priced 1.13 frames, 12 decodable passes exercise both sides: depth/velocity exceed their self curves while scene/f7/atlas/post/front sit below by 0.0001..0.0677; none crosses the 0.15 defect threshold. The prior raw-drop f7 verdict is contradicted at a measured 0.0117 deficit.

## Known failure modes

(none recorded yet)

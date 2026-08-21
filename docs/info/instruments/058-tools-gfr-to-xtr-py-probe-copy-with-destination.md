---
id: I058
kind: instrument
status: trusted
created: 2026-08-21
---

## Instrument

tools/gfr_to_xtr.py --probe-copy with destination-base-aware partial tiled decode

## Validated by

On walk_gameplay.gfr, the unchanged transform emitted a probe trace with SHA-256 3ac943a01e3fa2148e0e3e08005f04aa321c1adbd80437bd0c7cbba472faf95f before and after the module extraction. The other-answer arm moved the reused lower-tile copy to global Y and exposed the draw-612 depth plane under draw 650; omitting that transform read the wrong local rows. Scope: final live EDRAM state at an appended known-good copy, not the earlier checkpoint timing distrusted in I054.

## Known failure modes

(none recorded yet)

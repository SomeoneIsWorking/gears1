---
id: I028
kind: instrument
status: trusted
created: 2026-08-07
---

## Instrument

tools/draw_stream_compare.py over GEARS_DRAW_STREAM (ours) and GEARS_ORACLE_DRAW_STREAM (Xenia fork)

## Validated by

Both classes produced on real data: the set difference is EMPTY for vertex shaders (so it does not report a difference where there is none) and EIGHT for pixel shaders in one direction only (so it can find one). It also caught its own artifact -- the first run reported 33 unique pairs led by 680,525 draws, all of which were Xenia setting pixel_shader=nullptr on depth-only draws while we recorded the guest's programmed hash; normalising collapsed it to 10. Refuses on a missing file and on a side with zero frames. BLIND SPOTS, printed by the tool: it sees only WHICH shader ran and HOW OFTEN -- never constants, vertex data or textures -- and its per-frame count ratios mix scene compositions between runs of different lengths and are far weaker than the set difference. It records from , so a draw dropped before preparation is invisible.

## Known failure modes

(none recorded yet)

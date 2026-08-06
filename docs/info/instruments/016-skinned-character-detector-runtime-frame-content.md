---
id: I016
kind: instrument
status: trusted
created: 2026-08-06
---

## Instrument

skinned-character detector (runtime/frame_content.cpp, via GEARS_SKINNED_CHECK / GEARS_DRAW_FRAME_DUMP_SKINNED / tools/skinned_frames.sh)

## Validated by

Run against BOTH classes on 15 captures: FOUND on bright/black/play_v2/prison_turn, NONE on 11 including the 744-draw gameplay frames act1, courtyard, walk_gameplay, walk_v3 -- so it is not 'any large frame passes'. tools/skinned_frames.sh --selftest asserts the positive (bright.gfr) and the negative (courtyard.gfr) and PASSES. Verified live too: one 260 s run scanned 2614 frames and captured the first passing frame (character_auto.gfr, 12 skinned draws). KNOWN BLIND SPOT, printed in every negative: a character drawn without a GPU bone palette (CPU-skinned or morph-target) is invisible to it, and a shader whose microcode fails to analyse is counted separately as a blind spot rather than as a negative.

## Known failure modes

(none recorded yet)

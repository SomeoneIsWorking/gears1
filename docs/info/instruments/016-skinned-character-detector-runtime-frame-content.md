---
id: I016
kind: instrument
status: trusted
created: 2026-08-06
---

## Instrument

skinned-character detector (runtime/frame_content.cpp, via GEARS_SKINNED_CHECK / GEARS_DRAW_FRAME_DUMP_SKINNED / tools/skinned_frames.sh)

## Validated by

Run against BOTH classes on 16 captures: FOUND on bright/black/character_auto/play_v2/prison_turn, NONE on 11 including the 744-draw gameplay frames act1, courtyard and walk_gameplay -- so it is not 'any large frame passes'. tools/skinned_frames.sh --selftest asserts the positive (bright.gfr), the negative (courtyard.gfr), and the real mixed-result corpus-loop/reporting path: it must scan 2/2, count one positive and emit an explicit line for both classes. The full run prints all 16 names and a 5/16 denominator. Verified live too: one 260 s run scanned 2614 frames and captured the first passing frame (character_auto.gfr, 12 skinned draws). KNOWN BLIND SPOT, printed in every negative: a character drawn without a GPU bone palette (CPU-skinned or morph-target) is invisible to it, and a shader whose microcode fails to analyse is counted separately as a blind spot rather than as a negative.

## Known failure modes

Before issue #105, `set -e` made both the positive-count expression and the list-mode pipe terminate on the expected NONE result, so a nominal corpus run printed only act1.gfr. Fixed with explicit outcome handling; the selftest now exercises the shipping mixed-result loop and listing helper rather than only verdict().

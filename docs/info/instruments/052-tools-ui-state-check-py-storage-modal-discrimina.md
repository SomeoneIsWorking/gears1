---
id: I052
kind: instrument
status: trusted
created: 2026-08-14
---

## Instrument

tools/ui_state_check.py storage-modal discriminator

## Validated by

The discriminator is the shape of the UI draw run, not the frame-wide count. The real bad frame in scratch/camerapair_character_20260813/ours/draws.tsv has nine matches split as one baseline draw plus an eight-draw consecutive suffix; the clean repaired frame has one isolated draw, the clean turned frame has two isolated draws, and the clean static-world pair has two isolated draws. The shipping selftest therefore accepts one isolated draw, two isolated draws, zero draws, and nine isolated draws; refuses the measured one-plus-eight and eight-consecutive modal classes; and refuses an unknown three-consecutive class. The real bad table refuses after scanning 896 rows (9 matches, longest run 8), while the repaired real table passes after scanning 863 (1 match, longest run 1). Missing and empty tables still refuse.

## Known failure modes

This detects the measured storage modal, not arbitrary title-owned UI. A run of two through seven consecutive occurrences refuses as unknown rather than being guessed clean or modal. Zero occurrences are accepted because materially different scenes need not issue this shader pair at all; absence is not used as evidence that the scene is overlay-free.

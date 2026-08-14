---
id: I052
kind: instrument
status: trusted
created: 2026-08-14
---

## Instrument

tools/ui_state_check.py storage-modal discriminator

## Validated by

The discriminator is the shape of the UI draw run compared across both renderers, not the native frame-wide count. The real bad frame in scratch/camerapair_character_20260813 has native longest run 8 against oracle longest run 1 and refuses; the repaired pair matches at 1. Chapter 45 falsified the old absolute rule: a legitimate tutorial panel has native and oracle longest run 12, so classifying every run >=8 as the storage modal rejected equal state. The shipping paired selftest accepts equal run shapes 1 and 12 and refuses native 8 against oracle isolated runs. The older one-table mode remains available as a conservative diagnostic, but camera_pair uses the paired comparison. Missing and empty tables refuse.

## Known failure modes

The shader pair is shared by other title UI, including chapter 45's tutorial panel. Equality of longest run shape establishes only that this measured UI layer has the same structure; camera and pixel gates remain separately required. The oracle draw-order table is headerless while native draws.tsv has named columns, so the two parsers are independently exercised in the selftest.

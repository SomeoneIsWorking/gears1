---
id: I003
kind: instrument
status: trusted
created: 2026-08-05
---

## Instrument

tools/pass_structure.py (over GEARS_DRAW_DIAG, which now emits resolve rows)

## Validated by

Run on four captures totalling 2558 draws + 66 resolves with ZERO rows unattributed, and the attribution is self-checking in two ways: the two predicated EDRAM tiles of the courtyard frame classify as the SAME 174 base-pass draws with the SAME 44 pixel shaders in the same order, and the tile boundaries it reports are the resolve rows themselves (0x400->0xbdf0000 1280x512@0,0 then 1280x208@0,512) rather than something inferred. --selftest runs the classifier against 9 cases it must accept AND one it must REJECT (a colour-writing, depth-writing, depth-test-OFF draw must NOT be called a base pass); it is wired to fail on any verdict change. Its known blind spot is stated in its own output: UE3's lights, decals, distortion and translucency share one register state and are reported as one BLENDED band, not split.

## Known failure modes

(none recorded yet)

---
id: I001
kind: instrument
status: trusted
created: 2026-08-05
---

## Instrument

tools/verify_native_pass.sh (+ tools/compare_frames.py)

## Validated by

Run against BOTH classes on 2026-08-05: two arms of the same capture -> MATCH exit 0; two different captures -> DIFFERENT, mean |difference| 34.95, exit 1. It also refuses rather than reporting a match when an arm writes no screenshot, or when both frames are near-black. The trap it was built to close: the replay names its output after the frame number, so a hand-run comparison copied a STALE frame.ppm and compared a frame against itself, reporting a perfect match.

## Known failure modes

(none recorded yet)

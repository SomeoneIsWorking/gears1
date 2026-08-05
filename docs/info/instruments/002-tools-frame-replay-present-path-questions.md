---
id: I002
kind: instrument
status: trusted
created: 2026-08-05
---

## Instrument

tools/frame_replay present-path questions

## Validated by

DISTRUST for present questions on v1 captures. A v1 .gfr carries no front-buffer address, so the replay silently falls back to the last-geometry-draw rule and answers a different question than the live run. Capture format is now v2; v1 files still load and now WARN. Verified by observing 'front buffer 0x0' on every v1 replay while the live run has a real address.

## Known failure modes

(none recorded yet)

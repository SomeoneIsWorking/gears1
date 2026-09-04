---
id: C099
kind: claim
status: holds
created: 2026-08-28
tags: performance,audio,native-engine
depends: runtime/titles/gears1/audio_mix.cpp
---

## Claim

The native Gears 1 audio-mix kernel matched the output of original guest function
`0x825F2D40` for 256 historical same-call samples.

## Evidence

The recorded comparison observed no byte divergence across the 256 outputs. The
comparison harness no longer exists; dispatch and renewed qualification must run
through the authenticated `x360port` Xenia context.

## What would falsify it

Any same-input output divergence when the original guest function is executed
through Xenia, or evidence that the recorded address does not belong to the exact
authenticated Gears 1 revision.

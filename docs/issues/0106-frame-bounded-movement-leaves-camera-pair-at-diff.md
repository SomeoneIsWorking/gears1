---
id: 106
title: Frame-bounded movement leaves camera-pair at different permanent endpoints
status: resolved
symptom: A long camera-pair route fires every input step but native never reaches the frozen oracle camera
tags: oracle,pairing,input,determinism,camera
created: 2026-08-14
updated: 2026-08-14
---

Root cause: UE3 integrates movement over delta time, while the paired input interval was bounded by guest frame numbers. The oracle and native execute those frames at different wall-clock rates (catalog #84), so holding LY+ from f820 to f1500 moves them different physical distances. Once LY returned to zero, both endpoints were permanent and more runtime could not improve the camera distance. Negative evidence: scratch/camerapair_character_route_20260814 fired all ten native transitions and the oracle dumped 8/8 frames at f1981, but after native frame 7980 / 6300 qualifying held frames its closest result had stabilised at 3.09 thresholds (rotation 0.0154 against 0.005; relative translation 0.01436 against 0.013), with no capture. The harness then refused the absent draw table. The fix is route design, not a looser camera tolerance: leave forward movement active so native can cross the oracle's frozen path position at its own rate; the camera gate, not equal frame index, selects the corresponding moment.

The broader guest-time nondeterminism remains open on #84. This resolution only removes the permanent-endpoint mistake from camera-route acquisition.

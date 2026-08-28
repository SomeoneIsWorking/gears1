---
id: 155
title: Native scene composite gamma sign path diverges from translated shader
status: investigating
symptom: GEARS_NATIVE_PASSES=1 changes the first guest-texture composite when texture sign mode is kGamma
state_items: S004
tags: performance,native-rhi,native-pass,render,shader,gamma
created: 2026-08-28
updated: 2026-08-28
---

## Root cause

Not resolved. The native fragment module has the captured interface and executes
on the same draw stream, but its independently authored texture-sign path does
not yet reproduce the translated shader's `kGamma` behavior. The current
evidence narrows the defect to the gamma-sign execution contract: with signs
disabled, native versus translated is 0.2621 channel units mean absolute error;
forcing the hot shader's sign word to `0x3f` (gamma on RGB) leaves a 14.0665
channel-unit mismatch. The native module was removed rather than treating this
as a valid speed result.

## What was tried / dead ends

- A raw unsigned-sample probe changed the frame, proving the native module and
  its texture bindings execute; the first visible trace divergence is the
  guest-texture composite at trace row 98 (guest draw 112).
- Coarse derivatives were aligned with the translated `OpDPdxCoarse` and
  `OpDPdyCoarse` operations; this did not change the mismatch.
- The translated PWL constants and ordering were checked against Xenia's
  `PWLGammaToLinear`; changing the native RGB log/exp channel mapping did not
  remove the mismatch. This is not evidence that the gamma path is correct; it
  means the remaining cause may include its sampled-view/sign binding contract.
- Forcing `0x55` (signed on all channels) produced the same native and
  translated frame to 0.2621 mean error, so the retained A/B seam and the
  common signed-view path are not the large divergence.

## Resolution

Open. Reproduce with `scratch/frames/title600.gfr`, compare a fresh native-off
and native-on replay, and prove the actual sampled view plus sign byte for the
first divergent draw before implementing another native module. Falsifier:
the issue is resolved only when a same-input native/translated comparison
passes on a content-bearing frame with native texture signs enabled and the
wrong-sign control is rejected.

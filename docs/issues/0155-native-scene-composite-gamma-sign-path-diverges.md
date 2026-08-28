---
id: 155
title: Native scene composite gamma sign path diverges from translated shader
status: resolved
symptom: GEARS_NATIVE_PASSES=1 changes the first guest-texture composite when texture sign mode is kGamma
tags: performance,native-rhi,native-pass,render,shader,gamma
created: 2026-08-28
updated: 2026-08-28
state_items: S004
---

## Root cause

The root cause is resolved for the captured frame. The previous native fragment
module conflated two translated system-constant fields: it used the
host-swizzle word as the texture-sign mode as well as for channel routing. The
current independently authored module reads fetch-zero's sign byte from
`texture_swizzled_signs`; fetch routing is already composed into the Vulkan
image view by the host and is not applied again in the shader. Its PWL gamma
formula also now adds the truncated correction term at the correct scale.
With the corrected source, a fresh signs-enabled native/translated replay of
`title600.gfr` has 0.0000 mean channel error, 4/255 worst channel error, and
no differences above 4/255. A temporary deliberately wrong-sign control
diverged at 5.2155 mean channel units and 66/255 worst difference; this parity
result is not a speed claim.

## What was tried / dead ends

- A raw unsigned-sample probe changed the frame, proving the native module and
  its texture bindings execute; the first visible trace divergence is the
  guest-texture composite at trace row 98 (guest draw 112).
- The translated `SystemConstants` layout places `texture_swizzled_signs` at
  byte offset 64 (the first `uvec4` at word index 4) and `texture_swizzles` at
  byte offset 96 (the first `uvec4` at word index 6). The prior module read
  only word index 6 while deciding both behaviors.
- The live diagnostic sign probe decoded mode 3 in all RGB lanes on the first
  divergent draw. After correcting the gamma intermediate from
  `quantized * step + trunc(quantized * step)` to
  `quantized + trunc(quantized * step)`, the same-input replay matched.
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

Resolved after reproducing the first divergent draw, proving its mode-3 sign
state, correcting the PWL intermediate, and rejecting the temporary wrong-sign
control on the same captured input. The native pass is still limited to the
captured scene-composite contract and does not authorize a complete renderer
bypass or a performance claim.

### Resolution (2026-08-28)
Corrected the native PWL gamma arithmetic and kept texture-sign modes in texture_swizzled_signs separate from host-owned image-view routing; title600.gfr fresh signs-enabled A/B matched at 0.0000 mean channel error with 4/255 worst difference, while a temporary sign_word=0 control diverged at 5.2155 mean and 66/255 worst.

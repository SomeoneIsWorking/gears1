---
id: 73
title: A whole gameplay frame renders black: the bloom/DOF buffer is empty, so the post-process blend divides by zero
status: open
symptom: a captured gameplay frame renders completely black (0 of 921600 px non-black) although every draw issues and the scene colour target is full of content; other captures of the same scene render fine
tags: gpu,draw,post,bloom,dof,black,frame,native-renderer
created: 2026-08-05
updated: 2026-08-05
---

## Symptom

`scratch/frames/play_v2.gfr` renders **completely black** -- 0 of 921,600 px
non-black, while 921,600 px were written (so it is not an unwritten target). 656
of 868 draws issue, 0 skipped. `courtyard.gfr` and `bright.gfr`, the same game
area, render fine.

It has been black since before any of this session's changes; it is the capture
`tools/verify_native_pass.sh` has been calling INCONCLUSIVE as a negative control.

## Localised to one draw

Resolve dump (`GEARS_DRAW_RESOLVE_DUMP=1`) shows the world renders correctly --
scene colour `0xbdf0000` is 100% non-zero -- and the post chain then produces
zero: `0xc7f9000`, `0x6e4000` and the presented `0x311000` are all 0%.

Checkpoints (`GEARS_DRAW_FRAME_STEP`) put the transition at issued draw 631 =
**draw 840, pixel shader 0x9610bf8038af9aaf** -- UE3's uber post-process blend
(DOF/bloom composite + colour transform). Surface 0x2d0 holds 899,996 non-black
px before it and 0 after.

## What it is NOT

- **Not routing.** Forcing that pass to output solid red makes the final image
  solid red (33.3% of components non-zero = one channel). Its output reaches the
  resolve and the screen.
- **Not this session's native pass.** Native passes are OFF by default and the
  frame is black with the translated shader.
- **Not the scene colour.** It is full of content.

## The cause, measured

The pass takes three textures. Probed one at a time by making the native
reimplementation of that exact shader output each input in all three channels
(swap-proof -- the resolve applies `copy_dest_swap`, and a probe that writes one
channel gets its channels confounded, which cost an hour of contradictory
readings):

| input | play_v2 | courtyard |
|---|---|---|
| tf2 | **0.0% non-zero** | 1.8%, max 0.15 |
| tf0 (carries the picture) | 100%, max 1.0 | 100%, max 1.0 |
| tf1 (depth) | 100%, 0.0295..0.1626 | 100%, 0.0012..0.0826 |

tf2's numbers are exactly those of resolve target **0x6e4000** (0% on play_v2,
1.6% on courtyard), the 352x182 buffer the bloom/downsample chain on surface
0x5a0 writes immediately before this pass. So **tf2 is the bloom/DOF buffer and
it is empty**.

Why that turns the frame black rather than merely removing bloom: the shader's
decoded maths (see `runtime/shaders/uber_post_blend.frag`) is

    S = tf0 * sharpWeight + tf2.rgb
    W = sharpWeight + tf2.a
    A = saturate(S / W - Shadows)      ... and everything downstream is 0 if A is 0

`sharpWeight = saturate(1 - blurAmount)`. play_v2's depth range (0.0295..0.1626)
is far enough from the focus plane that `blurAmount` saturates, so
`sharpWeight = 0`; with tf2 also zero, **W = 0** and the divide takes the whole
frame to zero/NaN. On courtyard tf2 is 1.8% alive and the depth range is nearer
the focus plane, so W stays non-zero and the frame survives -- but its bloom is
almost certainly wrong too, just not fatally.

## Next

Find why the surface-0x5a0 -> 0x6e4000 chain resolves to zero. Its draws
(issued 626-630, ps a146058ecfeb9122 and bb4572ac75a8b550) all report `shaded`,
so they run and produce fragments; the content is lost between shading and the
resolve destination. Note the resolve is 328x184 into a 352x182 texture -- a
size mismatch worth checking first.

## Instrument fixed on the way

`GEARS_DRAW_FRAME_STEP` did not say WHICH surface each checkpoint dumped. A
frame switches surfaces several times, so a checkpoint going from 900k non-black
px to zero reads as "something wiped the frame" when it is only the target
changing to a small bloom buffer -- which is exactly how it was misread here
before the surface base was added to the line.

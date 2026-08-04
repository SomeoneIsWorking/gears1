---
id: 62
title: The 3D scene's colour is wrong: red is 78% of green frame-wide, and lit surfaces flatten at 0.30
status: open
symptom: Gameplay looks flat grey-green with blown, detail-free lit surfaces; menus and the title screen look correct in the same run
tags: gpu,draw,colour,tonemap,resolve
created: 2026-08-05
updated: 2026-08-05
---

## The observation that redirects everything

The reported title-screen capture is CORRECT -- dark blue, proper contrast, crisp
text. Only the 3D scenes are wrong, in the same run. That rules out every
whole-frame explanation I pursued for two days: a swapchain format, a colour
space, a compositor transform and an sRGB encode would all wash the menus too.

It is content-specific, which means it is in the part of the pipeline the menus do
not use: the deferred scene path, its HDR surface, the resolve that carries it and
the tonemap that consumes it.

## Measured on our own presented gameplay frame

Whole frame, per channel:  R 0.0772  G 0.0996  B 0.0990
                           -> RED IS 78% OF GREEN, and green and blue are equal.

The sunlit wall through the doorway: mean 0.193, p99 0.298, per-channel
R 0.174 G 0.204 B 0.200. A daylit concrete wall is neutral or warm; this is
uniformly cool, and it flattens at 0.30 with no highlight above it.

Two separate defects in one number: a RED DEFICIT of about a fifth, and a CEILING
that a lit surface cannot pass.

## What this retracts

Every "the renderer's output is correct" in this session was my own judgement of a
dim scene against no reference. It was wrong. The frames I called correct have the
same cast and the same ceiling as the reported screenshots -- the reports were just
of brighter scenes where it is obvious. Catalog #60 and #61 chased the difference
between my captures and the window when the interesting difference was between
BOTH of them and the console.

## Where to look, in order

1. The resolve's `copy_dest_swap` and the exponent bias -- catalog #33 found the
   bias wrong once already (the tonemap's input was 8x too bright), and a red/blue
   asymmetry is exactly what a swizzle or a per-channel scale gets wrong.
2. The 7e3 surface format carried as half-float: an unequal-precision packing would
   hit one channel differently.
3. The tonemap shader's own constants, read from the register file.

An oracle settles it in one comparison and there is one available: Xenia canary
renders this title correctly (user's report), and `extern/xenia` is already
vendored. A single frame of its output next to ours ends the guessing that this
entry exists because of.

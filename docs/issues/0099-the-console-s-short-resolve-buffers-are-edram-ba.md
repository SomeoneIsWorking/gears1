---
id: 99
title: The console's short resolve buffers are EDRAM bands, not partial writes
status: resolved
symptom: every full-screen console dump held 512 of 720 rows, and a second 1280x208 pass appeared as one only the console renders
tags: oracle,layer-compare,edram,resolve
created: 2026-08-11
updated: 2026-08-11
---

## What was measured

Every paired capture reported the console's full-screen destinations as short:

    srcC400 1280x720 f32 #0   [only 512 of 720 rows are in the console's buffer]
    srcD000 1280x720 f23 #0   [only 512 of 720 rows are in the console's buffer]

and listed two passes as ones only the console resolves:

    srcC400 1280x208 f32 #0
    srcD000 1280x208 f23 #0

## What it is

The two are the same pass. 1280x720 of colour plus depth does not fit in 10 MiB
of EDRAM, so the title resolves the frame in two horizontal BANDS -- 512 rows
and then 208 -- and their destinations are exactly contiguous:

    colour  12DD0000 + 1280 * 512 * 8 = 132D0000   (the band's own base)
    depth   13504000 + 1280 * 512 * 4 = 13784000

Our renderer collapses the EDRAM tiling (`GEARS_DRAW_TILED` restores it) and
resolves the whole surface once, so it has one destination where the console
has two.

## Why it mattered

The comparison was silently dropping the bottom 29% of every full-screen pass
-- including the scene colour and the scene depth, the two passes everything
else is judged against -- and reporting the second band as a pass the port
never renders. Both readings were wrong in the direction of "we are fine".

## Fixed

`layer_compare.py` rejoins them, on arithmetic alone: same source surface,
width and format, destination exactly one band of bytes past the previous one.
The aligned baseline is now 16 of 16 passes shared with ZERO one-sided, and the
scene colour and depth match over the whole 720 rows.

The remaining short buffer is real and is a different thing: the 864x864 shadow
atlas holds 672 of its rows because the copy writes a RECTANGLE into a larger
texture (catalog #97).

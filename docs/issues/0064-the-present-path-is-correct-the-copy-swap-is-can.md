---
id: 64
title: The present path is CORRECT: the copy swap is cancelled by the scanout format, so nothing should be swapped anywhere
status: resolved
symptom: gameplay looks cold blue-grey; the front-buffer resolve of the same frame is warm sepia and looks more like how Gears of War is described
tags: gpu,present,resolve,colour,dead-end,rgb
created: 2026-08-05
updated: 2026-08-05
---

## The tempting hypothesis, and why it is wrong

The renderer presents the EDRAM surface that fed the front-buffer resolve, not the
resolve's destination. The guest scans out the DESTINATION, and the resolve is not
a plain copy -- RB_COPY_DEST_INFO sets a red/blue swap on this title's front-buffer
copy. So 'present what the guest actually scans out' looks obviously right, and on
an Act 1 gameplay frame it is seductive:

    presented surface (today)      R 18.1  G 24.9  B 25.2   cold blue-grey
    front-buffer resolve of it     R 25.2  G 24.9  B 18.1   warm sepia

Same numbers, red and blue transposed. The warm version looks like the 'greys,
blacks and browns' Gears is known for.

## What settles it: the Epic Games logo

On the boot movie frame the two are the other way round -- the SOURCE surface is
warm and the front buffer is cold -- and there the correct answer is not a matter
of taste. The Epic Games logo is orange/gold on a red-brown background.

    source surface       the logo in its correct ORANGE
    front-buffer resolve the same logo in BLUE

Presenting the destination would invert the logo, and by the same rule every frame
in the game. The guest writes swapped bytes BECAUSE the scanout format reads them
back swapped; the two cancel. The image a person sees is the source surface, which
is what this renderer already presents.

## Cross-checked against real footage

An in-engine Gears of War reference frame (the 'Mad World' advert still, Wikipedia)
measures R 0.900 / G 1.087 / B 1.013 normalised -- RED IS THE LOWEST CHANNEL, and
the game's look is cold blue-grey, not sepia. Our shipped frame has the same
ordering (R 0.796 / G 1.096 / B 1.107); the swapped version inverts it (R 1.107 /
G 1.096 / B 0.796). Two independent checks, same verdict.

## Why this matters beyond one dead end

This is the SECOND time a red/blue swap has looked like the answer here. The first
attempt (#62) shipped a swap and was reverted after the user reported it fixed
nothing. Now the reason is known and general: **there is nothing to compensate
anywhere.** Any future 'fix' that swaps channels at any stage is undoing a swap
that was never wrong. The desaturated palette makes this trap self-reinforcing --
in a near-grey image, swapping R and B turns cyan-grey into sepia-grey, and BOTH
look like plausible Gears of War.

The rule for next time: settle a colour question on an image whose correct colours
are known independently (a logo, a UI element), never on a graded game scene.

## What was kept

The capture format gained the front-buffer address (v2). A v1 capture replays
correctly in every other respect but CANNOT reproduce the present decision -- it
falls back to the last-geometry-draw rule -- and it now says so loudly instead of
silently answering a different question. The frame report also names which rule
picked the presented surface; the old line claimed the guest's front-buffer address
chose it even when that address was zero.

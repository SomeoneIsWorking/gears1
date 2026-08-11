---
id: 96
title: Our HDR scene resolves are about fifteen times darker than the console's
status: open
symptom: layer_compare srcC2D0/srcC400 f32: ours 0.0032-0.0036, console 0.0537-0.0739
tags: oracle,layer-compare,hdr,resolve
created: 2026-08-11
updated: 2026-08-11
---

## What was measured

The comparer learned to decode k_16_16_16_16_FLOAT destinations (by reading the
buffers' real row count out of their lengths), and the three HDR scene resolves
are judged for the first time, on the SP_Prison_P paired capture with the EDRAM
sample model on:

    pass                        ours     console   mean |d|
    srcC2D0 1280x720 f32 #0     0.0036   0.0537    0.057
    srcC2D0 1280x720 f32 #1     0.0036   0.0739    0.077
    srcC400 1280x720 f32 #0     0.0032   0.0623    0.065   (512 of 720 rows)

Both sides are clamped to 0..1 for the comparison, because our resolve dumps
are 8-bit PPMs; the console's buffers reach 34400 and 58912 before clamping.
Even so ours are an order of magnitude below.

## Why this is worth its own entry

These are the scene colour buffers -- the input to the whole post chain -- and
until now the comparison could not read them at all, so every conclusion about
the frame's brightness came from the LDR end of the chain. Catalog #83 tuned
the reinterpretation pass on that basis.

## What it is not

Not the same thing as catalog #95: those are k_2_10_10_10 copies of the same
surface and are wrong in the opposite direction relative to the conversion
(ours too dark, and suppressing one conversion moves them the whole way). This
row set is the FLOAT read of the same memory, and no arm has been run on it.

## Caveat, stated

The console's 34400 and 58912 maxima are not obviously physical for scene
colour. The decode round-trips synthetic data and produces plausible means and
a plausible image, and three sibling passes at 352x182 still decode 1.4%
non-finite and are refused -- so the layout is right for these and not yet for
those. Treat the magnitudes as provisional until the 352x182 case decodes too.

### Note (2026-08-11)
THE 352x182 REFUSAL IS CORRECT, and now it is evidenced rather than assumed.

Decoding one of those buffers at 352x192x8 (its length, 540,672, is exactly
that) and looking at WHERE the non-finite pixels fall:

    columns with NaN, by index mod 16:  25 25 175 175 176 176 176 176
                                        18 19 19 20 18 20 18 20
    rows    with NaN, by index mod 16:  60 60 61 60 60 61 228 229
                                        55 54 56 54 54 54 55 55
    finite values range -61568.0 .. 61312.0, mean 1.92

Periodic in BOTH axes on a 16 stride, and a range no scene colour buffer has.
That is a tiling artifact, not a render target with NaNs in it -- so the 1%
refusal is refusing a real decode failure and not a real signal.

What separates this case from the 1280x720 ones that DO decode is the width:
1280 is 40 tiles of 32 and 352 is 11, so the odd tile count is where to look
first. Xenia's texture_util address has a term
(((y & 8) >> 2) + (x >> 3)) & 3) << 6 whose interaction with the row parity is
the natural suspect at 8 bytes per pixel, and the 8-byte path has only ever
been exercised here at width 1280.

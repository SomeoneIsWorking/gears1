---
id: 33
title: The resolve's copy_dest_exp_bias is ignored, so the HDR scene texture is 8x too bright
status: investigating
symptom: the assembled gameplay frame is heavily overexposed, large areas at 255; a captured Act 1 frame reads mean 208/255 on the top band and 187 on the bottom
tags: gpu,draw,draw-backend,resolve,hdr,tonemap,exposure,gameplay
created: 2026-07-28
updated: 2026-07-28
---

MEASURED, not guessed. The re-frontier named three candidates for the
overexposure -- the 7e3 encode/decode, the resolve's copy_dest_exp_bias, and the
k_16_16_16_16_FLOAT destination conversion -- and said to measure each. The
resolve census now decodes RB_COPY_DEST_INFO in full, and the first candidate it
was pointed at is the answer:

    draw 407: color@0x400 -> 0xbde0000  format 32 number 7 exp_bias -3
    draw 604: color@0x400 -> 0xc2e0000  format 32 number 7 exp_bias -3
    draw 647: color@0x2d0 -> 0xbde0000  format 32 number 7 exp_bias -3
    draw 670: color@0x2d0 -> 0xbde0000  format 32 number 7 exp_bias -3

Every resolve that writes the HDR scene texture carries copy_dest_exp_bias = -3.
That is a SIGNED 6-bit field (RB_COPY_DEST_INFO bits 16..21) and it scales the
colour by 2^bias on its way out of EDRAM -- so the guest is asking for the
resolved texture to hold src/8, and the tonemap pass that samples it is written
against src/8 values.

We ignore the field entirely. The resolved HDR texture therefore holds values
EIGHT TIMES what the guest's tonemap expects, and the composite blows out.

Note which resolves do NOT carry it: every k_8_8_8_8 destination (format 6) and
the k_16_16 one (format 25) have exp_bias 0, as do the three small
0x6e0000 bloom resolves -- also format 32, also number 7, but bias 0. So this is
not a blanket property of the format; it is per-resolve state the guest varies,
which is exactly why it has to be read rather than assumed.

THE FIX is not a blit. vkCmdBlitImage cannot scale values, and the destination
must hold the biased values because it is the guest's own shaders that sample it.
The resolve needs to become a scaling copy -- a small compute or full-screen
graphics pass that samples the source rect and writes src * 2^bias -- which is
also where the destination format conversion and the red/blue swap
(copy_dest_swap, set on two of this frame's resolves and also currently ignored)
belong. Xenia does exactly this in its resolve shaders.

NOT YET VERIFIED: that applying the bias fully accounts for the exposure. The
tonemap is non-linear, so an 8x input error does not map to a simple 8x output
error, and the other two candidates are untested. This entry records the
measured defect; whether it is the whole of the overexposure is settled by
implementing it and re-measuring, not by argument.

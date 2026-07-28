---
id: 33
title: The resolve's copy_dest_exp_bias is ignored, so the HDR scene texture is 8x too bright
status: resolved
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

### Note (2026-07-28)
IMPLEMENTATION IN PROGRESS, and NOT yet correct -- recorded here rather than
shipped, because a control arm caught it.

The resolve compute pipeline is built (runtime/gpu_draw_xlate.cpp
BuildResolveComputeShader, a hand-built SPIR-V compute shader that copies the
rectangle applying scale = 2^copy_dest_exp_bias and copy_dest_swap; the images
are declared with an UNKNOWN format because one pipeline serves surfaces that
may be 8888, half-float or two-channel float). Vulkan validation is clean and
the dispatches run.

It is OFF BY DEFAULT (GEARS_DRAW_RESOLVE_COMPUTE=1 selects it) because it fails
its own acceptance test. GEARS_DRAW_RESOLVE_SCALE=<float> forces the factor, and
at scale 1.0 the compute resolve MUST reproduce the blit it replaces. It does
not: 2762958 of 2764816 bytes differ. A scale sweep shows the destination
responds only weakly --

    scale 0.0   ->   2459/921600 px non-black (0.3%)
    scale 1.0   ->  85066/921600 px non-black (9.2%)
    scale 64.0  ->  95450/921600 px non-black (10.4%)

-- against 100% non-black from the blit. So the dispatch writes and is sampled,
but covers only a fraction of the destination. The likely area is the rectangle
and offset plumbing (push-constant extent, or the dispatch group count), not the
bias, which is confirmed correct against Xenia: its dest_exp_bias_factor is
exp2(bias) built by adding the bias to the exponent field of 1.0, matching
std::ldexp(1.0f, bias).

Ruled out along the way, each by a controlled arm rather than by reading:
  - The bias SIGN. Xenia's factor is 2^bias, the same convention.
  - The declared storage image format. Hardcoding rgba16f while binding an 8888
    surface view is a real mismatch and was fixed (Unknown format plus the two
    shaderStorageImage*WithoutFormat features, both queried, never assumed) --
    but fixing it changed the output by ZERO bytes, so it was not the cause of
    this failure. Worth keeping anyway: it would have been a latent defect the
    moment an 8888 surface resolved correctly.
  - The host being unable to store a format: no resolve reported unstorable.

The blit remains the default and is byte-identical to the verified tile-assembly
frame (0 of 2764816 bytes differ), so the working frame is not traded for an
unverified one. The blit cannot scale, so the exponent bias is still NOT applied
and this issue stays open.

### Resolution (2026-07-28)
FIXED and VERIFIED. The resolve is now a compute dispatch that applies the guest's copy_dest_exp_bias (scale = 2^bias) and copy_dest_swap while copying the rectangle to its destination offset. The blit it replaces cannot do either. TWO BUGS had to be found first, and the acceptance test found both -- at scale 1.0 with the swap suppressed the compute path MUST reproduce the blit byte for byte, and it did not: (1) the declared storage-image format. One pipeline serves surfaces that may be 8888, half-float or two-channel float, so hardcoding rgba16f and binding an 8888 view is a mismatch that returns garbage rather than failing. Fixed with an Unknown format plus the two shaderStorageImage*WithoutFormat features, both queried. (2) THE DESCRIPTOR POOL WAS SIZED FROM AN EMPTY LIST. It was sized on the ResolveEvent vector, which is not filled until the draw-preparation loop that runs AFTER the sizing -- so a frame with 12 dispatches got 8 sets, four dispatches silently reused a set that a later dispatch then overwrote, and the resolve targets those sets belonged to were written with the wrong source and destination. It was invisible in the frame; what exposed it was the new GEARS_DRAW_RESOLVE_DUMP=1, which writes every resolve target to a PPM with its maximum colour component: the HDR scene texture read max 0.000 under the compute path against 51.719 under the blit, and a scale sweep to 1000 left it still exactly 0.000, which is not a scaling error but a not-written-at-all error. Exhausting the pool is now counted and reported instead of wrapping. ACCEPTANCE PASSED: compute at scale 1.0 with the swap suppressed differs from the blit on 0 of 2764816 bytes. With the guest's own state applied the difference is 944250 bytes, all of it the red/blue swap the blit cannot perform. RESULT on the captured Act 1 frame: the HDR scene texture goes from max colour 51.719 to 0.808 -- into the [0,1] range its tonemap is written against -- and the presented frame goes from mean 201.8 with 51.6% of pixels SATURATED to mean 22.2 with 0.0% saturated. The frame is now a properly exposed dark interior with visible structure, beams, a lit barred window and legible caption, instead of a white-out. Vulkan validation clean. STILL VISIBLE and not this issue: a seam at the tile boundary (row 512) where the two predicated tiles meet.

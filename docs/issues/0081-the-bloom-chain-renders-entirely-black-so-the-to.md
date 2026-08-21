---
id: 81
title: The bloom chain renders entirely black, so the tonemap composites with no highlights
status: resolved
symptom: surface 0x5a0 has 0 non-black pixels after every one of its draws, and its resolve target is 0 of 192192 components non-zero
tags: gpu,draw,bloom,post,tonemap,colour,act1
created: 2026-08-06
updated: 2026-08-21
---

Found while chasing #62's narrow output range, on `scratch/frames/act1.gfr`
(the capture that reproduces it offline in 550 ms).

## Measured, two independent ways

The resolve dump: bloom's destination is empty.

    resolve target 0x6e0000 (352x182)  range 0.000000 .. 0.000000
                                       0 of 192192 components non-zero [0.0%]

And the checkpoint probe, per draw, on the surface that feeds it:

    checkpoint after 492 draws on surface 0x2d0: 825578 px non-black
    checkpoint after 493 draws on surface 0x5a0:      0 px non-black
    checkpoint after 494 draws on surface 0x5a0:      0 px non-black
    checkpoint after 495 draws on surface 0x5a0:      0 px non-black
    checkpoint after 496 draws on surface 0x5a0:      0 px non-black
    checkpoint after 497 draws on surface 0x5a0:      0 px non-black
    checkpoint after 498 draws on surface 0x2d0: 854590 px non-black

A pixel trace on 0x5a0 agrees: (100,50) is (0,0,0,0) for all 5 samples and
never changes.

## The draws are NOT being dropped -- they run and write black

From the diag table, guest draws 699/701/703 (issued #495/496/497):

    prim triangle_list, 6 verts, 2 prims after clip, **57600 fragment
    invocations**, verdict `shaded`, colour mask 15, blend off,
    viewport and scissor 322x182, surface 0x5a0 (k_16_16_16_16_FLOAT)

57600 of the 58604 pixels in a 322x182 rectangle. So the geometry, the raster
state and the colour mask are all fine, the pixel shader runs on essentially
every pixel of the target, and what it writes is zero.

That rules out the whole family of "the draw died at some stage" explanations
that catalog #30 and #31 covered -- this is a shader producing black, not a
draw that never happened.

## Why it matters for #62

Draw 707 (`ps 629226076307234e`) is the final full-screen pass into 0x2d0 before
the front-buffer resolve, and bloom is what puts the bright halo back into a
tonemapped image. With the bloom term identically zero, the top of the range has
nothing to come from -- which is exactly #62's symptom (our presented frame tops
out at 0.30-0.42 where the oracle reaches 1.0).

NOT YET ESTABLISHED: that fixing bloom fixes #62. Bloom being black is a defect
on its own terms and is stated as that.

## Next

`ps a146058ecfeb9122` is the bright pass: 161600 bytes of SPIR-V, 5 float
constants, 4 textures, 2 samplers, and the frame report shows it sampling the
DEPTH resolve destination 0xba40000. The question is what its four bindings
actually resolve to and which of them is black -- one of the frame's resolve
targets (0xcb81000) is legitimately all-zero because it resolves a
just-cleared surface, and if the bright pass reads that, it would produce
exactly this.

`GEARS_DRAW_PS_CONSTS=a146058ecfeb9122` printed nothing on this capture; that
needs looking at before it can be used to rule the constants in or out.

### Note (2026-08-06)
## The bright pass's constants, and an infinity that is the GUEST's (2026-08-06)

`GEARS_DRAW_PS_CONSTS=a146058ecfeb9122` on act1, once the knob was made to work
at all (see below):

    c[0]=(-9990.128, 0.0009999871, 0.1001001, 0.00010009881)
    c[1]=(1200, inf, 4, 0)          [44960000 7f800000 40800000 00000000]
    c[2]=(0, 0.4, 0, 0)
    c[3]=(0.5, 0.5, 0.5, 0.5)
    c[4]=(1, 0.0625, 0, 0)

**c[1].y is +infinity**, raw bits 7f800000. That is the shape of catalog #73 --
a post pass handed a value that makes its output degenerate -- so it was chased
the same way #73 was, and it lands the same way.

### The infinity is in guest memory, so the renderer did not make it

Searching the capture for the exact 16-byte block, big-endian:

    (1200, inf, 4, 0)   30 hits, at real guest addresses --
                        0x22ed0, 0x482b0, 0x4b0b0, 0x686c0, 0x8ae80,
                        0x8b9b0, 0xa8e30, 0xcf2a0, +22 more
    bare +inf dword     673 occurrences

Thirty copies of a well-formed constant block at guest addresses is the title
storing it deliberately, not a value we manufactured on the way in. Exactly the
exoneration #73 got, by the same instrument.

### And the arithmetic that would make it bite is already emulated

The Xenos rule that matters here is that a multiply by zero gives zero even when
the other operand is NaN or infinite, where IEEE and plain SPIR-V give NaN.
Xenia's SPIR-V translator implements it (`spirv_shader_translator_alu.cc`:
"Check if the different components in any of the operands are zero, even if the
other is NaN ... Replace with +0"), and we use that translator, so our shaders
inherit it.

So "the inf poisons the output through a multiply" is NOT available as an
explanation. NOT ruled out: that the shader reaches the infinity through some
other operation, or that it is meant to be consumed by a comparison whose Xenos
semantics differ. The constants are a lead that has been narrowed, not closed.

## Instrument fixed to get here: GEARS_DRAW_PS_CONSTS printed NOTHING

The printing lives inside the per-draw listing, which only runs under
`GEARS_DRAW_FRAME_LIST=1`. So asking for PS_CONSTS on its own produced no
output and no explanation -- the second knob in this session to answer a
question with silence.

It now pulls the listing in for the draws it names (and only those, rather than
a line per draw in the frame), and a hash that matched NO draw says so with the
frame's draw count, because "you asked about a shader this frame never ran" and
"the constants are all zero" both print nothing otherwise.

### Note (2026-08-06)
## WHY it is black, end to end, from the shader's own disassembly (2026-08-06)

The bright pass is `ps a146058ecfeb9122`, 459 dwords. Its shape, from
`xenos_translate --raw`:

    tfetch2D r2/r11/r9/r6/r8/r3/r4/r1.xyz_, r0.xy, tf0, Offset{X,Y}=...
    tfetch2D r0/r5/r12.x, r0.xy, tf1, Offset...
    sgt  r14.xyz_, r1.xyzz, c255.xxxx
    sgt  r10.x_zw, r4.xyyz, c255.xxxx
    sgt  r17/r18/r13/r7/r5/r19 ..., c255.xxxx
    max4 ...

It samples a 4x2 neighbourhood of tf0 (colour) and tf1 (depth), and every one
of those samples is put through `sgt <sample>, c255.x` -- set-greater-than
against a single scalar. That is the bloom THRESHOLD, and everything after it is
a max4 reduction of the results.

**c255.x = 1.0** (0x3f800000, measured on this draw).

**Its input tops out at 0.125.** fc0 binds resolve target 0xbde0000 with
`exp_adjust +0` -- so no sampling-side exponent compensation is asked for -- and
that target's measured range is 0.000000 .. 0.125000.

So every `sgt` compares something <= 0.125 against 1.0, every one yields zero,
and the pass writes black over its whole 322x182 target. Nothing is wrong with
the draw, the bindings, the raster state or the constants: the shader is doing
exactly what it was written to do with the numbers it was given.

## So this entry is a SYMPTOM, and the cause is upstream

Bloom being black is not a bloom defect. It is the first place where the scene
being too dark becomes VISIBLE as a hard zero rather than as a dim image, which
is why it showed up as "0 of 192192 components non-zero".

The question it hands upstream is sharp and quantitative: **the threshold is
1.0, and the input reaches it only if the surface behind it exceeds 8.0** (the
last resolve into 0xbde0000 is draw 670, from surface 0x2d0, with
copy_dest_exp_bias -3, i.e. x0.125). Our surface 0x2d0 maxes at exactly 1.0.
For this title's bloom to do anything on hardware, that surface must carry
values of at least 8, and ours carries 1.

Not yet known: whether 0x2d0 is capped at 1.0 by something we do, or is simply
receiving a scene that never gets bright. 46 of the draws feeding it are
colour_fmt 12 (k_2_10_10_10_FLOAT_AS_16_16_16_16), whose guest clamp is
alpha-only, so those draws are NOT clamped by us and could write above 1.0.

## Missing instrument

Every range measured in this investigation has come from a resolve DESTINATION,
because that is the only thing the renderer reports a range for. The question
above is about a SURFACE, and there is no probe that reports one's min/max. That
is the next thing to build, and it is why this stops here rather than guessing.

## New knob: GEARS_DRAW_TEX_BINDS=<ps hash>

What a named pixel shader actually samples, one line per binding: fetch
constant, base address, dimension, `exp_adjust` (with the multiplier it means),
and which of the three sources served it -- this frame's resolve target, a guest
texture, or a stub. The frame report only ever counted bindings by kind across
the whole frame, and three separate investigations have had to infer a single
pass's inputs from those aggregates.

### Note (2026-08-06)
Third instrument agrees the bloom surface is empty: GEARS_DRAW_SURFACE_RANGE reports surface 0x5a0 at 0.0000..0.0000 on every channel. And the upstream question this entry handed over now has a measured answer -- surface 0x400, the HDR scene, peaks at 2.19 with 0.19% of pixels above 1.0, where bloom's threshold implies it must reach 8.0. So the scene is real HDR but about 3.7x too dim, which matches the ~3.4x shortfall #62 measures at the other end of the pipeline.

### Note (2026-08-06)
## CORRECTION: bloom is NOT universally black -- it is act1 that has none

Ran the surface probe over every capture instead of just the one this entry was
opened on:

    capture      surface 0x5a0 (bloom) max R/G/B
    act1         0.0000 / 0.0000 / 0.0000        <- this entry
    act1_now     2.5488 / 0.6196 / 0.1220
    act1_v2      1.5840 / 0.4075 / 0.0751
    black        0.0268 / 0.0275 / 0.0258
    courtyard    0.0500 / 0.0500 / 0.0500

Bloom produces output in three of five captures, and above 1.0 in two of them.
So the chain works, and "the bloom chain renders entirely black" is wrong as a
general claim -- it is true of act1 only.

Everything measured about act1 stands: the bright pass thresholds with
`sgt <sample>, c255.x` at c255.x = 1.0, its input tops out at 0.125, so it
writes zero. What changes is the reading: on act1 that is bloom CORRECTLY
finding nothing above the threshold in a dark moment, not a defect.

This entry was opened on a single capture and generalised from it. The
discriminator existed the whole time -- run the same probe on a capture that
should bloom -- and running it is what settles it.

Status: not a defect on the evidence available. Left open only because it is
unknown whether act1's moment SHOULD have bloom; the oracle could answer that
and has not been asked.

### Note (2026-08-12)
REPRODUCES LIVE AT A MATCHED CAMERA, so the 'stale act1.gfr v1 capture' explanation that parked this issue does not cover it. In scratch/camgate/match -- a live headless capture gated to the console's own view-projection -- all three bloom resolve destinations (srcC5A0 352x182 f32 to 0x0c3a0000, draws 1121, 1123, 1125) are IDENTICALLY ZERO: 0 of 192,192 components non-zero on each. The console at the SAME camera (oracle frame 571, copies 8/9/10 to 0x1389C000, same 352x182 f32 pass) carries 1.44%, 1.66% and 1.81% non-zero BYTES with a maximum byte of 255. The console-side check is at the BYTE level and needs no decoder, so it stands even though layer_compare refuses the 8-byte f32 decode on those three passes (909 of 64,064 decoded pixels non-finite -- a decode that failed, reported as such rather than as a difference). AND IT NOW HAS A QUANTIFIED COST: catalog #62's remaining deficit, measured on the same capture, is 2.0x at the median and 3.5x at p90 with p99.9 and max intact -- the exact shape of a missing broad low-level additive term and not of a missing highlight. This makes #81 a contributor to #62 rather than a cosmetic issue, and gives it an acceptance gate: fix it and re-run tools/front_buffer_percentiles.py on this same pair.

### Note (2026-08-12)
THE BLOOM INPUT IS 3.2x TOO DIM AND SO IS THE FRAME'S FIRST RESOLVE, SO THIS IS NOT A BLOOM DEFECT AND NOT A POST-CHAIN ONE. Measured at the matched camera with tools/front_buffer_percentiles.py, ours against the console's raw guest bytes from the frame our capture was gated to. THE BLOOM'S OWN INPUT (srcC2D0 1280x720 f32 to 0x0c7c0000, our draw 1064, against oracle_f571_copy6 to 0x12DD0000): ours max R 0.4549 G 0.3451 B 0.3020; theirs max R 1.4453 G 1.2500 B 0.7109. The bright pass thresholds every sample with sgt against c255.x = 1.0, which this entry established from the disassembly. THE CONSOLE'S INPUT CROSSES THAT THRESHOLD IN TWO CHANNELS AND OURS CROSSES IT IN NONE -- so the console's bright pass finds something and ours finds nothing, and both shaders are behaving correctly on the numbers they are given. That is this entry, quantified, with the reference finally on the other side of the comparison. AND THE SAME SHORTFALL IS ALREADY PRESENT AT THE FIRST RESOLVE OF THE FRAME. The scene resolve srcC400 1280x720 f32 at our draw 638 -- the earliest colour resolve in the frame, long before any post pass -- against oracle_f571_copy0: ours max R 0.4196 G 0.3412 B 0.3020, theirs R 1.4453 G 1.2520 B 0.7129. Identical ratio, 3.4x. The second composite resolve (our draw 1092 against copy7) repeats it: 0.4549 against 1.4375. THREE RESOLVES SPANNING THE WHOLE FRAME, ALL SHORT BY THE SAME FACTOR, THE FIRST OF THEM BEFORE ANY POST PROCESSING RUNS. So the loss is in the scene pass or in the resolve of it, and everything downstream -- black bloom here, the midtone deficit in #62 -- is a consequence. This entry's own 2026-08-06 note said 'this is a SYMPTOM and the cause is upstream'; that reading is now confirmed against a reference rather than inferred, and the upstream point is located at the frame's first resolve. INSTRUMENT LIMIT, STATED BECAUSE VERIFYING A FIX WILL HIT IT: our resolve dumps are 8-bit PPMs, so they CANNOT represent a value above 1.0. That does not weaken this result -- our maxima are 0.42-0.45, i.e. 107-116 of 255, with no pile-up at 255, so nothing was clipped -- but the moment a fix pushes us above 1.0 the dump will clamp and read 255, and the comparison will silently stop working. A float dump of resolve destinations is needed before the fix can be verified. The MEDIAN comparison is also quantization-limited here (both sides sit below 1/255) and should not be quoted from this pair.

### Note (2026-08-12)
The two preceding notes' RATIO numbers are retracted -- the camera-gated pair fails a same-picture check (log-luminance correlation 0.07, best 0.16 over flips and shifts, against 0.93 for a pair that must agree), so 'the console's bloom input is 3.2x brighter than ours' measures the pairing rather than the renderers. See #62's retraction note for the full evidence. WHAT STANDS, because it needs no moment match: our three bloom resolve destinations in scratch/camgate/match are IDENTICALLY ZERO -- 0 of 192,192 components on each -- while the console's three at the same structural pass carry 1.44%, 1.66% and 1.81% non-zero BYTES with a maximum byte of 255. That is a presence check, not a comparison of values, and it holds whatever moment either frame shows. So this entry still reproduces live on a fresh capture and the 'stale act1.gfr' explanation still does not cover it. WHAT FALLS: the quantified claim that the console's input crosses the 1.0 sgt threshold and ours does not. That reading came from decoded VALUES of an unpaired frame. The disassembly result behind it -- the bright pass thresholds every sample with sgt against c255.x = 1.0 -- is from the microcode and is unaffected; what is not established is where either side's input actually sits relative to it.

### Note (2026-08-13)
THE FRESH PROVENANCE-MATCHED PAIR CONFIRMS THE BLOOM PRESENCE DEFECT; THE NEXT QUESTION IS HDR, NOT A MAGIC EXPOSURE FIX. `scratch/camerapair_current` shares one pair id and frozen camera and passes the front-buffer gate at console frame 873 (1.9 frames of the console's own drift). Our three C5A0 f32 resolves at draws 1161/1163/1165 contain zero of 192,192 RGB components and zero of 64,064 pixels non-zero. The corresponding oracle f873 copies 13/14/15 each contain 10,473 non-zero bytes of 540,672, maximum 255. That zero-versus-nonzero result is independent of the f32 decoder. WHAT IS NOT ESTABLISHED: whether our bright-pass input failed its guest threshold or the pass differs another way. An 8-bit PPM cannot distinguish 1.0 from 20.0. `GEARS_DRAW_RESOLVE_DUMP_FLOAT=1` now captures the exact mapped RGBA16F resolves and `tools/resolve_float_stats.py` refuses malformed/non-finite payloads while reporting the >1.0 denominator. Use a walk that actually reaches the frozen camera before changing the bloom shader or an exposure constant; an attempted runtime-only run without that walk stayed hundreds of thresholds away and produced no capture.

### Note (2026-08-13, paired HDR retry)

THE FIRST HDR RETRY WAS ALSO NOT A MEASUREMENT: `camera_pair.sh` supplied no native pad input, so the oracle captured frames 871–875 while our headless runtime remained at the title screen (no frame reached the 400-draw minimum, no resolve or raw-float file was written). This was a harness defect, not evidence about bloom. The script now sources `tools/menu_walk.sh` and defaults `GEARS_INPUT_SCRIPT` to that maintained walk, while preserving an explicit caller override. The sandboxed `gpuguard status` report of `unknown / NOT RUNNING` was also not a health result: it cannot access the user systemd bus. An out-of-sandbox status check showed `gpu-guard.service` active, running `gpuguard.py watch` since 20:39 with no kernel fault.

### Note (2026-08-13, raw native files are not a pair)

`scratch/hdr_current_20260813` contains real raw RGBA16F output from a nearby native run: the scene resolve peaks at 0.4055, the C2D0 f32 resolve immediately before bloom peaks at 0.8623, and all three C5A0 bloom resolves are exactly zero. Thus that native run had no sample above the bright shader's 1.0 threshold. It is NOT oracle comparison evidence: the directory has no `PROVENANCE.json`, and its frame differs from `camerapair_current/ours` (only 66.0% of sampled pixels identical, although mean absolute error is 0.37/255). The console's f32 resolve layout is also still refused by `layer_compare.py` on real dumps: its own synthetic decoder passes but real C5A0 rows contain non-finite values, so a made-up decode cannot supply the missing console HDR values. Do not derive an exposure multiplier from this. A newly provenance-stamped HDR pair remains the acceptance gate.

### Note (2026-08-13)
THE 2026-08-13 FRESH-PAIR BLOOM CONCLUSION IS RETRACTED: scratch/camerapair_current is provenance-matched and camera-close, but it is not UI-state matched. The native log says `[input] no input source (headless, no GEARS_INPUT_SCRIPT); the pad reports disconnected`, and its final resolve visibly contains the NO STORAGE DEVICE modal. Native draws 1193-1200 are eight blended dialog draws (VS 5363d0746b3ef666 / PS 501ac5d8692bf7b6) after motion blur; oracle frame 873 has the same bright-pass, two blur, composite, and motion-blur shader suffix but no equivalent dialog draws before its final resolve. Therefore the drop at native draw 1201 and the zero-versus-nonzero bloom observation come from a pair with different guest/UI state and are not renderer evidence. The deeper harness cause was asymmetric input policy: the oracle was explicitly launched with an empty schedule while native was supposed to use a millisecond walk, so even commit 698050b only repaired one half. `camera_pair.sh` now generates the oracle and native syntaxes from the same frame-indexed `GEARS_WALK_TABLE` and refuses unless both runtimes log that they accepted their schedule. Existing `layercap3` logs prove the passing class on both sides; `camerapair_current` proves the refusing class. The nearby raw-HDR run remains native-only and unpaired. The acceptance gate is unchanged: a new provenance-stamped HDR pair made with the shared walk, and its images must be inspected for matching overlays before pixel/pass conclusions are accepted. No new GPU run was made in this session.

### Note (2026-08-21)
2026-08-21: The current exact-state chapter-45 bloom chain is non-black, and its bright-pass output already matched the synchronous oracle. A separate first-blur defect was found and resolved as #114: guest pitch 352 had been used as the normalized sampled width instead of the fetch constant logical width 322. All three current C5A0 passes now have 0.00% of pixels differ by more than 0.1. This closes the remaining bloom-path defect; earlier notes that generalized a black capture were already retracted.

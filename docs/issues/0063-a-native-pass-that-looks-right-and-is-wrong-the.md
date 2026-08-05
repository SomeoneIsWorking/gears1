---
id: 63
title: A native pass that looks right and is wrong: the bindings, not the maths
status: resolved
symptom: our own shader draws the pass with correct geometry but wrong colours; the microcode arithmetic was transcribed correctly
tags: gpu,shaders,native-renderer,descriptors,spirv
created: 2026-08-05
updated: 2026-08-05
---

## Symptom

The first native pass (movie YUV->RGB composite, hash 0xea0007942db096ad) rendered
the Epic logo with correct geometry and structure but green/pink instead of
rust/white. Mean |difference| against the translated pass: 40.3 of 255.

## What it was NOT

Not the arithmetic. Re-deriving the microcode confirmed the original transcription
was right in every respect: the dp3 operand order (r2.zxyy = (V,Y,U) against
c*.zxyy), and the three colour offsets (c0.w, c1.w, c2.w) * c3.x, which look
asymmetric in the listing only because r0.y comes from the vector pipe while r0.x
and r0.z come from the scalar pipe's previous-scalar chain (maxs leaves c0.w in PS,
muls_prev multiplies it).

## Cause

The DESCRIPTOR BINDINGS. The shader is substituted into a pipeline the translator
laid out, so its interface is dictated, not chosen. For three texture fetches the
translator emits:

    set 3: binding 0/1 = texture0 unsigned/signed
           binding 2/3 = texture1 unsigned/signed
           binding 4/5 = texture2 unsigned/signed
           binding 6,7,8 = the three samplers
    set 1: binding 0 = system constants, binding 2 = pixel float constants

The native shader had declared textures at 0,1,2 and samplers at 3,4,5. That is not
a validation error: it sampled texture0's signed view and texture1 as if they were
the U and V planes, and bound samplers onto texture bindings. The result still drew
a recognisable picture. **A wrong binding fails silently and plausibly**, which is
why this cost a session and the arithmetic cost nothing.

Three further interface facts, all read off the translated module with spirv-dis:

- XeFloatConstants is sized to the constants the shader touches -- four vec4s here,
  not 256. A larger array reads past the buffer.
- color_exp_bias is a vec4 at offset 192 of the system constants, not a float[4].
- Every translated pixel shader ends with an epilogue the native pass must also
  perform: multiply by the render target's exponent bias, then, when system flag
  bit 14 (kSysFlag_ConvertColor0ToGamma) is set, a piecewise-linear gamma encode.
  It is the 360's PWL curve, NOT sRGB. Omitting it makes every pixel the wrong
  brightness while the image still looks like the right image.

## The last 4 pixels: dot() is wrong by one ULP

With bindings fixed, 2764796 of 2764800 channel samples matched exactly and four
were low by one. Not a coordinate problem -- isolated interior pixels. GLSL dot()
may be fused or reassociated by the compiler; the sequencer evaluates
(x*a + y*b) + z*c. Declaring the product and sum `precise` forbids both transforms
and closed it to bit-exact.

## Trap hit while verifying

The replay names its screenshot after the frame number, so a capture that writes
frame_00600.ppm leaves an earlier frame.ppm in place. The hand-run negative control
copied the stale file and compared a frame against ITSELF, reporting a perfect
match and 'proving' the comparison could not distinguish two different captures.
Closed by tools/verify_native_pass.sh, which deletes the output before every arm,
fails if none reappears, and runs the negative control in the same invocation.

## Method that worked, for the next pass

1. scratch/shaders/bound_out/<hash>.ucode.txt for the microcode.
2. spirv-dis on the matching .spv for the INTERFACE (bindings, block layouts,
   member offsets, the epilogue). Trust these offline modules for structure only --
   they are translated with no modification key, so they have no interpolator
   inputs and a colour write mask of zero, and their behaviour is meaningless.
3. Write the GLSL; tools/gen_native_spv.sh to regenerate the header.
4. tools/verify_native_pass.sh until it is bit-exact.

---
id: C001
kind: claim
status: holds
created: 2026-08-05
tags: gpu,native-renderer,shaders
depends: runtime/shaders/movie_yuv.frag, runtime/native_pass.cpp
---

## Claim

Our own shader can replace a title pixel shader bit-exactly: the movie YUV->RGB composite renders 2764800/2764800 channel samples identical to the translated microcode

## Evidence

tools/verify_native_pass.sh on scratch/frames/boot150.gfr: mean |difference| 0.0000, worst channel 0, exact 2764800 of 2764800; negative control (act1.gfr) reports mean |difference| 34.95 in the same run, so the comparison is shown able to say DIFFERENT

## What would falsify it

any nonzero worst-channel from tools/verify_native_pass.sh; and note this is ONE capture of ONE pass -- it says nothing about passes with signed fetches, exponent bias, or more than four float constants, all of which movie_yuv.frag takes the straight path on

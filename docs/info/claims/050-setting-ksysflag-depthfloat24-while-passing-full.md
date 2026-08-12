---
id: C050
kind: claim
status: holds
created: 2026-08-12
tags: 
---

## Claim

Setting kSysFlag_DepthFloat24 while passing full_float24_in_0_to_1=false made every oDepth-writing pixel shader emit half-scale depth; removing it fixes the shadow masks and is invisible to correlation

## Evidence

Xenia's convention has two halves that must agree: the viewport half (draw_util.cc:553, full_float24_in_0_to_1 halves z_min/z_max to remap [0..2) float24 into [0..1)) and the shader half (spirv_shader_translator_rb.cc:1512, CompleteFragmentShader_DSV_DepthTo24Bit multiplies oDepth by 0.5 'as viewport scaling doesn't apply to oDepth'). We passed false at both call sites (gpu_draw_xlate.cpp:1268, :1470) and set the flag anyway. BEFORE: median depth ratio console/ours 2.000057 over 655,360 px, 98.11% within 1% of exactly 2.0. AFTER: mean 0.044691 vs console 0.044691, median ratio 0.999986. Correlation is scale-invariant so the depth pass scored 0.9847 throughout and the error was invisible; the DEPTH TEST is not, so under reverse-Z GEQUAL a halved buffer over-fired zpass marks and under-fired depth-fail volumes. Confirmed bidirectionally: the ZPASS mark went DOWN 60,368 -> 18,266 against the console's 18,098 (same shader, same 10,292 prims, joined by signature not ordinal). Mask #1 went from a flat 1.0 to 33 distinct values and 4.85% shadowed against the console's 33 and 4.88%; mask #0 to 11.35% against 11.39%. Every pass in the frame is now at or above its own temporal yardstick.

## What would falsify it

any change to gpu_draw_xlate.cpp's viewport Setup calls (full_float24_in_0_to_1 or convert_z_to_float24), or a capture where our depth resolve's median differs from the console's by more than 2%

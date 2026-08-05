---
id: C006
kind: claim
status: holds
created: 2026-08-05
tags: gpu,native-renderer,ue3
depends: runtime/shaders/base_pass_lightmap.frag, tools/verify_native_pass.sh
---

## Claim

A UE3 base-pass material shader is tractable as a native pass, and its channel rotations must be reduced by simulating the register file rather than by reading swizzles

## Evidence

runtime/shaders/base_pass_lightmap.frag reimplements pixel shader 0x1f1a3f779667a02a (49 ALU instructions, 7 texture fetches, 5 interpolators) and is bit-exact against the translated shader on two independent captures: 2,764,800 of 2,764,800 channel samples on courtyard.gfr and on bright.gfr, each with a negative control reporting a difference in the same run and zero Vulkan descriptor/shader-interface warnings. THE MATCH IS NOT VACUOUS: the base pass renders into EDRAM and only reaches the compared image through two resolves and the whole post chain, so the pass was deliberately broken (output halved) and the comparison re-run -- 473,625 of 2,764,800 channel samples changed, worst channel 28. The reduction was obtained by symbolically simulating the register file (every instruction applied to named expressions, common subexpressions interned) rather than by composing swizzles by hand; the microcode has ten consecutive accumulations each rotating channels by one, and all of those permutations cancel while the ORDER of the six accumulation steps does not.

## What would falsify it

a second base-pass material that resists the same procedure -- e.g. one whose channel permutations do NOT compose to the identity, or one using an instruction the simulation does not model (it has no control flow, no memexport and no scalar ops beyond rsq/adds/maxs/mulsc/muls_prev)

---
id: 68
title: Widening a reinterpreted surface to a float host format loses the render target's [0,1] clamp, and translucent UI blends as a saturated slab
status: resolved
symptom: the main menu's selected entry is a flat white bar with its label illegible; the unselected entries render correctly
tags: gpu,blend,render-target,edram,colour,fixed
created: 2026-08-05
updated: 2026-08-05
---

## The defect, visible

The menu's selected entry ('NEW CAMPAIGN') rendered as a solid white rectangle, min
255 max 255 across the whole bar, swallowing the label drawn over it. The two
unselected entries were correct. A shipped game does not ship an illegible selected
menu item, so this is wrong regardless of what the intended styling is.

## The cause

The Xbox 360's render target FORMAT decides what happens to a shader's colour
output before blending: a fixed-point target clamps it to [0,1], a floating-point
one does not.

This renderer breaks that link. The guest reinterprets EDRAM surface 0x2d0 between
k_8_8_8_8, k_2_10_10_10_FLOAT, k_16_16 and k_2_10_10_10_FLOAT_AS_16_16_16_16 within
a single frame, and the render-target cache gives it ONE host image in a container
wide enough for all of them -- necessarily R16G16B16A16_SFLOAT. Every draw into
that surface then blends as if its target were HDR.

The UI highlight is a translucent white gradient. Its shader (ps 0xc1857858203fec94,
texture 0x1cb7000, k_DXT4_5, pure white RGB with a real alpha ramp) computes

    oC0.w   = texAlpha * c2.x * (c1.x*c255.x + c255.y)   = texAlpha * 0.2715
    oC0.xyz = colour * c255.z                            = colour * 8

The x8 is the guest's HDR convention. On a fixed-point target the 8 clamps to 1 and
the blend gives a soft highlight; on a float target it survives, 8 * 0.15 = 1.2
saturates, and every texel of the bar comes out at 255 -- the alpha gradient is
destroyed entirely.

## How it was proved before it was fixed

GEARS_DRAW_FORCE_LDR=1 (control arm) collapses the surface to R8G8B8A8_UNORM so the
hardware clamp happens again. That renders the bar as a gradient: mean 58.3, min
44, max 124, against the shipped 255/255/255. It also destroys the HDR passes, so
it is a diagnostic, never a fix.

## The fix

runtime/spirv_clamp.cpp inserts an NClamp of the fragment colour output to [0,1]
into the translated SPIR-V, for exactly those draws whose GUEST colour format is
fixed-point (k_8_8_8_8, k_8_8_8_8_GAMMA, k_2_10_10_10, k_2_10_10_10_AS_10_10_10_10)
while the HOST image has been widened to a float container. It is not applied when
the guest's own format is floating-point -- that would clip the HDR passes the
widening exists to support -- and not to k_16_16/k_16_16_16_16, whose fixed-point
range is -32..32 rather than [0,1].

The clamp is part of the shader cache KEY, because two draws can share a microcode
and a modification and still need different modules. It applies to native-pass
modules too: exempting them would make a native pass diverge from the translated
shader on exactly these draws, and the A/B gate would blame the native pass.

Result, against the control:

    before (float host)      bar mean 255.0  min 255  max 255   frame 97.9% non-black
    after  (shader clamp)    bar mean  58.2  min  44  max 124   frame 97.9% non-black
    control (UNORM host)     bar mean  58.3  min  44  max 124   frame 97.6% non-black

The fix matches the hardware control to 0.1 while KEEPING the HDR content the
control loses. The label is legible. An Act 1 gameplay frame also loses ~12% of its
mean brightness (R 18.1 -> 15.9), which is the same overbright leak being clamped
where the console would have clamped it.

Both native passes remain bit-exact through the gate, and 24 tests pass including a
new one for the transform, whose most important case is that a module it cannot
handle is REFUSED rather than left half-rewritten.


## Companion case: 7e3 alpha (added after the fix above)

`k_2_10_10_10_FLOAT` and `k_2_10_10_10_FLOAT_AS_16_16_16_16` are the awkward
formats: RGB is a 7e3 float running to 32, but **alpha is a 2-bit UNORM**, so the
console clamps alpha to [0,1] and leaves colour alone. The widened host format
loses that too, so the transform gained an alpha-only mode and those draws now use
it. Clamping all four components there would flatten exactly the HDR range the
widening exists to carry, which is why it is a separate mode rather than the same
one.

**Measured honestly: it changed no pixel** on any of the three captures
(act1_v2, act1, boot150 all identical to two decimal places). It costs one extra
shader variant and zero extra pipelines on an Act 1 frame. It is kept because it is
the same root cause and the same documented hardware behaviour as the fix above,
not because it was observed to matter — if it is ever seen to matter, it will be a
draw writing alpha outside [0,1] into a 7e3 target.

The 16-bit fixed formats (`k_16_16`, `k_16_16_16_16`) are deliberately excluded
from both modes: their fixed-point range is -32..32, so a [0,1] clamp would be a
new defect rather than a fix.

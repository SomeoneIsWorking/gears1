---
id: 67
title: GEARS_DRAW_FRAME_STEP never wrote a single image: the checkpoint was taken with a null target
status: resolved
symptom: per-draw checkpoint dumps produce no files and no log lines, indistinguishable from a frame with nothing to report
tags: instruments,gpu,diagnostics,attribution
created: 2026-08-05
updated: 2026-08-05
---

## Three faults, all silent

GEARS_DRAW_FRAME_STEP=N is meant to write the colour target after every N draws so
a defect can be attributed to a DRAW instead of guessed at. It has never produced
an image. Three separate faults, none of which reported anything:

1. **The checkpoint was taken with a null.** The call site is

       endPass();
       checkpointHere(drawn, openTarget);

   and endPass() sets openTarget = nullptr. checkpointHere's first line returns on
   a null target. Every checkpoint in the project's history hit that return. Fixed
   by reading the target into a local BEFORE endPass().

2. **It only accepted 8888 surfaces.** 'An HDR surface's bytes are not pixels', so
   it returned -- and every surface in this title's frames is
   R16G16B16A16_SFLOAT. Fixed by blitting into an 8-bit staging image, the same
   conversion the presented frame already gets. The staging image is its OWN, not
   one of the present pair, because a diagnostic must not be able to corrupt what
   the user sees.

3. **The 48-checkpoint cap truncated silently.** With STEP=1 it covers draws 1-48
   and stops, which reads as full coverage of the frame. It now warns with the
   count it dropped, and GEARS_DRAW_FRAME_STEP_FROM=M aims the window -- without
   it a late-frame defect (UI, post chain) is unreachable at any step size.

Verified: 49 checkpoints written on a 157-draw frame where the previous build
wrote zero.

## What it immediately found

The menu's selected item ('NEW CAMPAIGN') renders as a solid white bar with its
label nearly illegible, while unselected items are legible white-on-dark. Six
successive quads (draws 147-152, ps 0xc1857858203fec94) ramp that region from mean
31.5 to EXACTLY 255.0.

Traced as far as the evidence goes:

- the texture is fine. 0x1cb7000 is k_DXT4_5, decodes to pure white RGB with a
  real alpha gradient (115 distinct values, ~137/255 mid-bar). Uploaded as
  VK_FORMAT_BC3_UNORM_BLOCK, so the driver decodes it.
- blend state is standard alpha (SRC_ALPHA / ONE_MINUS_SRC_ALPHA, ADD).
- the shader is oC0.w = texAlpha * c2.x * (c1.x*c255.x + c255.y) and
  oC0.xyz = colour * c255.z. Its constants, read with GEARS_DRAW_PS_CONSTS:
  c1 = 0.89251363, c2 = 1, c255 = (-0.2, 0.45, 8, 0).
  So alpha = 0.54 * (0.45 - 0.1785) = ~0.147 -- and COLOUR IS MULTIPLIED BY 8.

With src.rgb = 8 and src.a = 0.147, one draw already contributes 8*0.147 = 1.18,
which clips to white. The x8 is the guest's HDR bookkeeping (the scene composite
scales by 8 too, and the scene's own resolve carries exp_bias -3 = /8). The open
question is where that x8 is supposed to be cancelled before an 8-bit front buffer,
and whether this renderer cancels it in the right place.

**NOT YET A PROVEN DEFECT**: the same arithmetic on real hardware would clip the
same way, so the highlight may be white by design with dark text on top -- in which
case the defect is the text colour, not the bar. What is established is the draw
range, the shader, the constants and the texture, so the next session starts from
four measured facts instead of a frame.

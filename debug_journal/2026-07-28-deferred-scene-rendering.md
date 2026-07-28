# 2026-07-28 — Getting the deferred in-game scene to render

At the start of the day the title booted, played its menus and reached Act 1
gameplay, but the in-game frame was a flat grey field with a correct HUD floating
on it. By the end it is a properly exposed, tile-assembled, depth-lit interior at
~32 ms/frame. Nine defects were found. **Six of them were in code written during
this session, and every one produced output that looked plausible.**

That last fact is the point of this entry. The individual fixes are in the
catalog (`catalog.py show 29`..`37`); what is worth carrying forward is how they
were found, and how nearly they were missed.

## The loop that was not working

The first hours were spent A/B-ing environment knobs — one 200-second scripted
menu walk per hypothesis. That is not merely slow. Two such runs land on
*different game moments*, so the two arms of a comparison were never comparable,
and two wrong suspects for the missing world had already survived being
"measured" that way.

The fix was to stop guessing and build the instrument:

- **`GEARS_DRAW_FRAME_DUMP` + `tools/frame_replay`** — record a frame's whole
  draw stream (register snapshots, deduplicated microcode, non-zero guest pages)
  and re-render it offline in ~550 ms with no guest. Every arm then runs on
  byte-identical input.
- **`GEARS_DRAW_DIAG`** — one row per draw joining what it *was* with what it
  *did* (pipeline statistics) and every piece of state that can silently zero it,
  with a `verdict` column naming the stage it died at.
- **`GEARS_DRAW_RESOLVE_DUMP`** — write every resolve target to a PPM with its
  maximum colour component. The resolved textures are what the guest's post
  passes *sample*; "is the scene in there, and does it look right there?" cannot
  be asked of the presented frame.

The diagnostic table answered in one run what a day of runs had not: 389 of 391
world draws were **killed at clipping**, not shading black.

## The defects, and why none of them looked like a defect

The **missing world** was the guest-memory mirror: 64 MiB, while the frame
fetches vertices to 237 MiB. A fetch past the mirror reads *zero*, so every
vertex collapses to the origin and the primitive dies at clipping. It looks
nothing like a memory bug and exactly like a broken transform.

The **white-out** was `copy_dest_exp_bias = -3` on the HDR resolves, ignored, so
the tonemap's input was 8× too bright.

The **haze over everything** was our own diagnostic slate clear. It was chosen so
that any lit pixel was obviously guest geometry — and it is not black, so it
lifted the 7e3 HDR surface off zero, and a constant floor under a bloom chain is
fog over the whole image. *An instrument that participates in the thing it
measures has stopped being an instrument.*

The **seam between predicated tiles** was a clear tied to a copy succeeding. The
guest clears depth once per tile, riding on that tile's depth resolve — and the
depth resolves are exactly the ones we could not serve, so the clears were
discarded along with them. Two unrelated-looking gaps were one line of code.

## The rule that earned its keep

**An acceptance test that the new code must pass before it is allowed to be the
default.** The compute resolve had to reproduce the blit it replaced, byte for
byte, at scale 1.0 with the swap suppressed. It failed twice, and both failures
were real: a storage-image format mismatch, and a descriptor pool sized from a
list that was still empty (12 dispatches, 8 sets, four silently reusing another's
descriptors). The frame looked plausibly darker throughout. Without the test it
would have shipped.

The same shape recurred: `OpImageFetch` without its `Lod` operand returns zero
rather than failing; a dispatch bound descriptor sets allocated with the wrong
layout and read nothing; a census recorded the *previous* draw's shader hash and
sent me disassembling a shader with no texture fetch in it.

**And the instrument itself was wrong first.** The resolve dump read every target
as half-float, so it reported `0.000` for an R32_SFLOAT depth target regardless
of content — indistinguishable from "the resolve wrote nothing". Three real bugs
sat behind that one reading. An instrument that cannot tell "wrote nothing" from
"I am reading it wrong" is worse than none.

## Two gaps that were not gaps

Twice a number that read like structure evaporated under measurement:

- "Only 24 of 5278 texture bindings are served by a resolve target" — classified,
  ~5224 are ordinary world textures, correctly uploaded, and 0 fall inside a
  target. 24 render-target samples per frame is normal. Retracted.
- "One shared depth image against 4 `RB_DEPTH_INFO` bases" — both major surfaces
  genuinely share base `0x0`; only 8 of ~737 draws use another. A refactor for a
  rounding error.

Measure before building, including — especially — when the thing sounds
structural.

## What is deliberately not done

The per-format unpack of `RB_COLOR_CLEAR`. Every colour clear this title has ever
programmed, across every captured run, is `0x00000000` — 478 of them. A decode
would produce identical output whether right or wrong, so it is counted,
reported, and falls back rather than guessed. If a frame that clears to something
else appears, the report says so and the unpack can be written against it.

The frame is not claimed faithful. It is `re-partial` on the re-frontier with its
remaining gaps named and sized, because "it looks right" is not evidence and
there is no console to diff against.

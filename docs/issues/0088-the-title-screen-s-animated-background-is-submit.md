---
id: 88
title: The title screen's animated background is submitted on 8 frames of 1581, not every frame
status: open
symptom: our title screen is a flat grey-brown where the console's is bright orange fire; at guest frame 600, same moment on both, our mean red is 0.1045 against the oracle's 0.2508 while green is 0.0533 against 0.0713
tags: render,oracle,title-screen,gameplay-scene,guest-logic
created: 2026-08-07
updated: 2026-08-07
---

FOUND BY THE FIRST TRUE SAME-MOMENT COMPARISON. tools/oracle_lockstep.sh now
drives both sides from one frame-keyed walk table (catalog #87) and numbers both
filmstrips by the guest's present counter, so guest frame 600 is the title
screen on both sides -- logo, "PRESS START", and the copyright line pixel-aligned
between them. No moment mismatch is available to explain this one away.

    ours   mean RGB 0.1045 0.0533 0.0431   R/G 1.962
    oracle mean RGB 0.2508 0.0713 0.0499   R/G 3.518

Our red is 42% of the console's while green is 75% and blue 86%. That is not a
channel scale: the console's screen carries a bright animated fire layer over
the grunge texture, and ours carries the grunge texture alone.

THE MECHANISM, from the same-frame draw-stream diff (GEARS_DRAW_STREAM_RAW here,
GEARS_ORACLE_DRAW_STREAM on the fork). Seven (vs, ps) pairs are bound by the
console at frame 600 and not by us -- about 11 draws:

    vs 58ddc62747c4203b  ps 183b49ec32221496   2 draws
    vs 58ddc62747c4203b  ps dd4cc810ceb76ede   2 draws
    vs 58ddc62747c4203b  ps 7d8f22586ca8be17   2 draws
    vs 58ddc62747c4203b  ps b1838912be34636e   1 draw
    vs c38e883ea10eee9a  ps f1b813590fc50b44   2 draws
    vs 15dbe06e53bd0f02  ps 32091b4c63cda933   1 draw
    vs 0330603173ed375c  ps 0000000000000000   1 draw  (depth-only)

WE DO NOT DROP THEM AND THE SHADERS ARE NOT MISSING. The raw stream records at
the top of the draw loop, above every drop site, and reports 0 pairs programmed
and never prepared at frame 600. Our guest DOES program all seven -- just almost
never:

    ours    5 to 12 frames each, out of 1,581, at 566, 679-681, 753-769,
            1249-1262, 1477, 1502 -- scattered bursts, never two long runs
    oracle  314 to 333 frames each, out of 904, on the CONTIGUOUS span 571..903
            -- i.e. every frame the title screen is up

So this is a GUEST-SIDE divergence, not a renderer one: the title's animated
background pass is issued by the game code on roughly 0.5% of the frames it
should be. The seven pairs almost always fire together, so it is one coherent
pass group being gated, not seven independent draws going missing.

NOT YET INVESTIGATED -- what gates the group. Candidates in order:
  1. an update driven by guest TIME (a UE3 scene-capture or material-animation
     tick). Catalog #84 establishes our guest clock is host real time and that
     four attempts to virtualise it failed, so a time-gated update is the first
     thing to rule in or out;
  2. an update gated on a fence, occlusion query or GPU-progress value we answer
     differently;
  3. a streaming/decode completion the title waits on.

WHY THIS IS THE BEST TEST CASE IN THE PROJECT. It is reachable in ~20 seconds
with no gameplay walk, it needs no determinism, both sides are pinned to the
same moment by the title screen itself, and the difference is large and obvious
rather than a percentage. Every previous cross-emulator finding here has been
weakened or withdrawn over moment mismatch; this one cannot be.

SEPARATELY, in the same diff and worth its own look: at frame 600 the same
vertex shader 760aacf6212e632c splits 50 draws with a pixel shader / 5 without
on our side, and 2 with / 53 without on the console's -- the same 55 draws
classified oppositely. Our runtime decides "has a fragment stage" from
RB_MODECONTROL.edram_mode == kColorDepth alone; Xenia's IssueDraw applies THREE
conditions, and we implement only the middle one:
  a. draw_util::IsRasterizationPotentiallyDone(regs, polygonal) -- if false
     Xenia SKIPS the draw entirely (memexport aside);
  b. edram_mode == kColorDepth;
  c. draw_util::IsPixelShaderNeededWithRasterization(shader, regs) -- false when
     the shader does not kill pixels, does not write depth, has no memexport,
     and every colour target it writes is fully masked by RB_COLOR_MASK for that
     target's component count. That is a Z-prepass, and the console runs it with
     no pixel shader at all.
So we run a pixel shader on ~48 draws per title frame that the console runs
depth-only. Whether that changes any pixel depends on whether our colour write
mask is honoured; it is at minimum wasted work and it is a faithfulness gap.

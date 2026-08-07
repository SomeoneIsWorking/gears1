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

### Note (2026-08-07)
ROOT CAUSE FOUND AND FIXED (title screen). Every occlusion query was answering
"nothing was visible".

runtime/vd_null_gpu.cpp's EVENT_WRITE_ZPD handler zero-filled the whole
xe_gpu_depth_sample_counts record. D3D computes an occlusion result as
END.ZPass - BEGIN.ZPass (A and B summed), so an all-zero record is not a
neutral answer -- it is a positive report that no pixels passed, and the title
believes it and culls. The handler's own comment justified it: "A GPU that
rasterises nothing has zero samples in every counter." That was true when it was
written. It stopped being true when the renderer started drawing, and neither
the code nor the comment came back.

It also explains the shape of the symptom exactly. The first title frame renders
the group because no query has reported yet; from the second frame the results
say invisible and it is culled; periodic re-tests produce the scattered bursts
(566, 679-681, 753-769, 1249-1262, 1477, 1502) rather than a clean off.

THE FIX, A/B ON THE SAME SCREEN, same walk, same frames:

    GEARS_GPU_ZPD_ZERO=1 (the old answer)  post group on   11 of 562 frames (2.0%)
    default (monotonic ZPass)              post group on  563 of 563 frames (100.0%)

    draws per frame, median   161 -> 171   (console 173)
    title screen mean red     0.0863 -> 0.1571   (console 0.2508)
    title screen R/G          1.79 -> 2.68       (console 3.52)

The screen goes from grey-brown to red. Not parity: the console's fire is
brighter still and its shape is not reproduced, so something in that pass group
remains wrong -- but the group now runs on every frame instead of 2% of them,
which is a cause removed rather than a symptom moved.

WHAT THIS DOES NOT FIX, measured rather than assumed. In GAMEPLAY the pass group
was already present on 100% of frames on BOTH arms (6,091 frames on the control,
4,014 on the fixed run, 100.0% each). So this is a title/menu-screen fix and the
gameplay scene's darkness (#62) is NOT explained by it. A run under the fix does
show Marcus rendered where earlier runs showed no character, but that is NOT
attributable here: the control run's walk ended facing a wall, so the two runs
were at different positions and the comparison does not support the claim. The
character question stays open on #77.

STOPGAP, and marked as one in the code. Reporting a fixed "everything visible"
is not measuring occlusion. The proper fix is a real Vulkan OCCLUSION query pool
around the draws between the BEGIN and END events, resolved back into this
record. It is a stopgap over a WRONG answer rather than over a missing one, and
it errs in the only safe direction: over-reporting visibility costs drawing
something hidden, under-reporting deletes geometry the title asked for.

### Note (2026-08-07)
SECOND GAP CLOSED: the fragment-stage classification now matches the console's.

draw::ClassifyDraw (runtime/gpu_draw_xlate.cpp) calls Xenia's own
draw_util::IsRasterizationPotentiallyDone and
draw_util::IsPixelShaderNeededWithRasterization against the analyzed pixel
shader this backend already caches, and gpu_draw.cpp applies both alongside the
edram_mode test it had. Measured at guest frame 600 against the console:

    before   vs 760aacf6212e632c ps 63c971f5e9d59913   ours 50   theirs  2
             vs 760aacf6212e632c ps 0000000000000000   ours  5   theirs 53
    after    that pair no longer differs at all

Draws issued with no fragment stage went from 15 to 64 per title frame, and in
gameplay from 789 to 837 of 2,256. Nothing was skipped for non-rasterisation (0)
and no pixel shader failed to analyse (0), on both screens.

WHAT IT DID NOT DO, stated because the reason for making the change was
faithfulness and it would be easy to imply more. It does not visibly improve the
title screen: mean R/G went 2.851 -> 2.495 against the console's 3.518, i.e. it
moved slightly and not clearly closer. The two runs are different phases of an
animated screen so a small difference is expected either way. The value here is
that our draw classification now matches the console's exactly, which removes it
as a candidate for every remaining difference, not that it fixed one.

Undecided is a THIRD state and is counted, not folded into "no". A pixel shader
that fails to analyse leaves the question to the edram_mode test and increments
drawsClassifyFailed, which the mode census always prints -- including its zero.
Answering "no fragment stage" on our own failure would silently turn every draw
of a broken shader into a depth-only pass, which looks exactly like the
console's own Z-prepass and would be invisible in every diagnostic here.

GEARS_DRAW_MODE_ONLY=1 restores the mode-only test as the control arm.

STILL OPEN on this issue: the title screen is closer but not at parity -- the
console's fire is brighter and its shape is not reproduced. With the draw set
and the classification now both matching, the remaining difference is in what
those shared draws PRODUCE, not in which draws run.

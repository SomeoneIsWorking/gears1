---
id: 97
title: The console clears a fifth of the shadow atlas that we leave at far depth
status: open
symptom: srcD5A0 864x864 f22 depth resolve: mean |d| 0.196 overall, but 0.0084 over the 78.9% the console does not clear
tags: oracle,layer-compare,depth,shadow,clear
created: 2026-08-11
updated: 2026-08-12
---

## What was measured

The layer comparison can decode depth destinations now, so the two shadow-map
resolves are compared for the first time. Overall they differ:

    srcD5A0 864x864 f22 #0   ours 0.8980   console 0.7095   mean |d| 0.196
    srcD5A0 864x864 f22 #1   ours 0.8485   console 0.6663   mean |d| 0.227

Splitting the atlas by what the console holds there:

    the console is near zero (< 0.01) over 21.1% of it; we hold 0.894 there
    over the OTHER 78.9%:  ours 0.8989   console 0.8997   mean |d| 0.0084

So the shadow maps themselves AGREE -- 0.0084 is a match by any standard used
here -- and the whole of the reported difference is a region the console clears
to ~0 (near, under reverse-Z) and we leave at far depth. The side-by-side
(scratch/layercap_e/layers2/pass_D5A0_864x864_f22_0.png) shows it: the same
hanging bodies and the same stairwell in the same atlas tiles, with a black
block and a black bar along the console's bottom edge where ours is white.

## Why it matters

It is a CLEAR we are not performing, not a rendering difference, and a shadow
atlas whose unused tiles read "far" instead of "near" changes what every lookup
into them returns. It is also the last thing catalog #91 had left as a suspect
for the mask's geometry -- and this rules the map's content out, because the
content matches.

## Where to look

The atlas is 864x864 and the console's buffer holds 672 of those rows, so the
cleared region is partly the rows beyond 672 and partly a block inside them.
A resolve that also clears (RB_COPY_CONTROL.color_clear_enable, bit 8) is the
first candidate: the runtime decodes depth_clear_enable (bit 9) and acts on it,
and does NOT decode colour clears at all -- gpu_draw_resolve_decode.cpp reads
only bit 9, and the frontier notes a non-zero colour clear has never been
decoded because every one this title programs is 0x00000000. A clear to zero is
exactly what this looks like.

### Note (2026-08-11)
THE PREMISE IS WRONG: no clear is involved, and this is mostly not a defect.

The copies that write the shadow atlas do NOT program a colour clear -- the
frame's resolve census says "clears: none" for every one of them, and only four
copies in the whole frame clear anything. What the region actually is: a copy
writes a RECTANGLE into a texture (the atlas copy is 448x448 into an 864-wide
destination), and the rest of the destination is whatever the guest's memory
already held, which is zeros. Our side is a host image whose unwritten area
holds whatever it holds. Neither renderer wrote there, so comparing there says
nothing about either.

The comparison now reports both numbers on every depth row, and the shadow maps
read:

    #0  whole destination |d| 0.196; over the 78.9% the console did not leave
        at zero, |d| 0.0084 -- they agree where both wrote
    #1  whole destination |d| 0.227; over the 78.8%, |d| 0.0439

So #0 is not a finding at all. #1's 0.0439 over the written region is above the
0.02 the tool calls a match and is the only part of this worth keeping -- one of
the two shadow-map copies differs where both sides wrote, and the other does
not.

Retitled in effect: what remains is "one shadow-map copy differs by 0.044 over
its written region", which is a much smaller and much better-posed question
than the one this entry opened with.

### Note (2026-08-12)
REPLICATED AT A MATCHED CAMERA, AND THE AGREEMENT IS TIGHTER THAN MEASURED HERE. A capture gated on the console's own view-projection (GEARS_DRAW_FRAME_CAMERA, matched at 3.77) resolves the same 16 passes as the console with none only-ours and none only-theirs. For the shadow atlas copy #0 it gives whole-buffer means 0.8983 ours against 0.7095 theirs -- the same numbers this issue records -- and over the 78.9% the console does not clear, mean |d| 0.0031 against the 0.0084 measured here. Per tile: (5,5) 0.9692/0.9690, (437,5) 0.9668/0.9673, (5,437) 0.5481/0.5479, (226,437) 0.3813/0.3821. Four tiles agreeing to three decimal places. The better alignment improved the agreement, which is what should happen if the residual was frame drift and not a rendering difference -- so this issue's conclusion that the map CONTENT matches is confirmed independently, and the missing clear is the whole of it. Our own atlas draws carry the tile rects as scissors (5,5 422x422; 437,5 422x422; 5,437 211x211; 226,437 211x211) and our only atlas-wide draws are the clear quads at scissor 0,0 880x1440, six before the first resolve and four between the two. Since the console clears MORE than we do, not less, the question is what it clears the outside fifth WITH -- the fork now logs PA_SC_WINDOW_SCISSOR with every draw of the dumped frame (fork a54abbc) so the console's own clear rects can be read off directly.

### Note (2026-08-12)
THE SCISSOR IS NOT THE MECHANISM, so the clear VALUE is. The fork now logs PA_SC_WINDOW_SCISSOR with every draw and a console frame's 1,091 draws carry these rects: 404 at 0,0 1280x720; 310 at 0,512 1280x208; 310 at 0,0 1280x512; 48 at 0,0 16x16; 11 at 0,0 8192x8192; 5 at 5,5 422x422; 3 at 0,0 322x182. The clear-shaped draws -- the ones with an 8192x8192 viewport -- carry an 8192x8192 SCISSOR on the console, and the same draws on our side carry 0,0 880x1440. But 880x1440 fully contains the 864x864 atlas, so our clear is not being cut short: both sides cover the whole atlas. That rules out the scissor, which was the obvious suspect and is now eliminated rather than left hanging. WHAT IS LEFT is the value. Both sides clear the same region and the region ends up at ~0 on the console and 0.894 on ours, so the difference is WHAT the clear writes, not WHERE. Under reverse-Z the console's result is NEAR and ours is FAR. The next check is what depth value the guest's clear specifies for this pass and whether our clear path honours it -- a register and a code path, not a geometry question. CAVEAT ON THE MEASUREMENT, because it matters: this console frame is gameplay+15, not the aligned moment, and its atlas has one 422x422 tile active rather than four. It is sound for the question asked -- whether any draw carries the 880x1440 rect, and none does -- but it is NOT a per-draw pairing and must not be read as one.

### Note (2026-08-12)
ROOT CAUSE LOCALISED: OUR CLEAR RASTERISES NOTHING ON THIS SURFACE, AND ONLY ON THIS SURFACE. Every one of the twelve clear draws that target the shadow atlas -- ordinals 981, 982, 984, 985, 987, 988, 1005, 1006, 1041, 1042, 1045, 1046, all vertex shader 760aacf6212e632c, depth test on, depth write on, depth func 7 (ALWAYS), viewport 8192x8192 or 16384x16384, scissor 0,0 880x1440 -- assembles its primitives, survives clipping with 2 to 6 of them, and produces ZERO FRAGMENT INVOCATIONS. A clear that rasterises no fragments writes nothing, which is exactly why the region stays at far depth. THE SHADER IS NOT BROKEN AND THE COMPARISON PROVES IT BOTH WAYS. Frame-wide our side runs 69 draws of that shader, assembling 75 primitives, keeping 102 after clip and producing 58,604 fragments -- more than the console's 16,476 across 59 draws. So the clear works everywhere in the frame EXCEPT on the atlas. And measured directly on the console with a pipeline-statistics query, its draws of the same shader produce 16,476 fragments where ours produce zero on the twelve that matter. THE VIEWPORT CLAMP IS NOT IT: the guard at gpu_draw.cpp:1538 never fired in this capture, so 8192 and 16384 both fit the device's 16384 limit. But that guard tests gv.w against vpMaxW while the applied value at line 1563 is min(gv.w * sx, vpMaxW) -- the check and the clamp are on different quantities, so a draw with a sample scale of 2 would be silently halved with no warning. That is a real defect in the instrument even though it is not this bug. WHERE TO LOOK NEXT: 0x5a0 is a NON-ZERO DEPTH BASE, and with GEARS_DRAW_SPLIT_DEPTH off every depth base collapses to 0. The atlas clear carries a scissor of 880x1440 which is the atlas surface's own size; if the target it actually lands on is the collapsed base-0 depth target with different dimensions, the scissor and the geometry no longer agree and nothing rasterises. That makes this issue a consequence of the depth-base work (C031) rather than an independent defect, and it predicts the clear starts writing under GEARS_DRAW_SPLIT_DEPTH=1.

### Note (2026-08-12)
THE PREDICTION IS FALSIFIED: GEARS_DRAW_SPLIT_DEPTH=1 DOES NOT MAKE THE CLEAR WRITE. Two arms of the same length, same content gate, only the knob differing. SPLIT=0: 76 draws of clear shader 760aacf6212e632c, 81 primitives assembled, 114 after clip, 58,604 fragments; of those, 16 target depth base 0x5a0, assembling 20 and keeping 40 after clip, with ZERO fragments. SPLIT=1: 59 draws, 60 assembled, 72 after clip, 58,604 fragments; of those, 2 target 0x5a0, assembling 2 and keeping 4, again with ZERO fragments. So the atlas clear rasterises nothing either way and the depth-base collapse is NOT the mechanism. The note above that predicted otherwise was wrong and is retracted; the measurement it was built on -- zero fragments, only on that surface -- stands and is unaffected. THE TEST IS WEAK AND MUST NOT BE OVERSOLD: the two arms landed on different moments, 16 atlas clear draws against 2, so they are not the same frame and the SPLIT=1 arm rests on two draws. What it does establish is that turning the knob on does not turn the fragments on, which is what the prediction claimed. Worth noting that the fragment TOTAL is identical at 58,604 in both arms while the draw counts differ by 17 -- so every draw that differs between the arms contributes no fragments, which is consistent with the differing draws being exactly these dead clears. WHERE TO LOOK NOW: the surface is not the mechanism and neither is the shader, so the remaining candidates are the target's own dimensions at the moment these draws are recorded, and the relationship between the 880x1440 scissor and whatever extent the atlas render target actually has. The next instrument is the render target's dimensions logged alongside the scissor for each of these draws -- 'the scissor and the target disagree' is checkable directly and has not been checked.

### Note (2026-08-12)
SHARPER AND CORRECTED: IT IS EVERY NON-ZERO DEPTH BASE, NOT JUST THE ATLAS. Grouping our frame's 83 draws of clear shader 760aacf6212e632c by the depth base they target: base 0x0, 55 draws, 58,604 fragments -- every fragment in the frame. Base 0x5a0, 23 draws, ZERO. Base 0x2d0, 4 draws, ZERO. Base 0x400, 1 draw, ZERO. So the clear rasterises if and only if it targets depth base 0, and my earlier 'only on that surface' was too narrow: 28 draws across THREE non-zero bases all produce nothing. THE PIPELINE STATE IS IDENTICAL ACROSS BOTH GROUPS -- every one of the 83 draws carries vte_cntl 0x300 and clip_cntl 0x10000 -- so viewport-transform enable and clip control do not distinguish the ones that work from the ones that do not. THE VERTEX DATA DOES. These are PRE-TRANSFORMED WINDOW-SPACE quads: vertex 0 of a live draw reads (-0.5, -0.5, 0, ..., w=1) and vertex 1 (639.5, -0.5, 0, ..., w=1), i.e. a 640-wide quad in pixel coordinates. A dead draw's vertices read (-0.5, -0.5, -3.7252907e-09, ..., w=0) and (639.5, -0.5, -3.7252907e-09, ..., w=0). THE FOURTH COMPONENT IS ZERO ON THE DEAD ONES. With vte_cntl 0x300 the guest is supplying window coordinates with a reciprocal-w, and a w of 0 makes every primitive degenerate the moment it is treated as a clip-space w -- which is exactly zero fragments from primitives that survive clipping. THAT IS THE MECHANISM AND IT IS TESTABLE: what does vte_cntl 0x300 mean for the w component, and does our translated vertex shader pass it through as clip w where the hardware would substitute 1? Xenia's own handling of PA_CL_VTE_CNTL's w0_fmt bit is the reference. This is no longer a depth or clear question at all -- it is vertex-shader output semantics for pre-transformed vertices, and it would explain a class of missing draws well beyond this atlas.

### Note (2026-08-12)
STOP: ZERO-FRAGMENT CLEAR DRAWS ARE NORMAL. THE CONSOLE DOES IT TOO, AND MORE THAN WE DO. Of the console's 59 draws of clear shader 760aacf6212e632c in its frame, 57 produce ZERO fragment invocations and only 2 produce any (16,476 between them). On our side 55 draws produce all 58,604 fragments and 28 produce none. So we rasterise clear quads on 55 draws where the reference rasterises on 2 -- WE PRODUCE MORE, NOT LESS -- and the 28 'dead' draws I was treating as the defect are matching the reference's dominant behaviour. The mechanism recorded above is NOT established and the w=0 reading does not survive: our runtime already sets the same three PA_CL_VTE_CNTL system flags Xenia does (kSysFlag_XYDividedByW, kSysFlag_ZDividedByW, kSysFlag_WNotReciprocal, gpu_draw_xlate.cpp:1274-1276), so there is no divergence in how the pre-transformed w is handled, and a w of 0 producing no fragments is evidently what the hardware does as well. What remains true and measured: the atlas region the console clears to near and we leave at far, which is this issue's original symptom and is untouched by any of today's mechanism hunting. THE LESSON, AND IT IS THE THIRD TIME TODAY: an inference of the form 'our number looks wrong' collapsed as soon as the console's distribution of the SAME quantity was measured. It happened with post-clip primitive counts (235 against 21,296 became 21,111 against 21,296 once the cameras matched), with the atlas region (framed as us writing where we should not, when it is a clear we do not perform), and now with the clear's fragments. NEVER conclude a defect from our side's numbers alone; measure the reference's DISTRIBUTION of the same quantity first. The instruments to do that exist now -- GEARS_ORACLE_PRIM_STATS gives per-draw counts and takes 90 seconds.

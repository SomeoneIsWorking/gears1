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

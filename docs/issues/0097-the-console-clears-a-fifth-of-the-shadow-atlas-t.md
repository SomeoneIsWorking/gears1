---
id: 97
title: The console clears a fifth of the shadow atlas that we leave at far depth
status: open
symptom: srcD5A0 864x864 f22 depth resolve: mean |d| 0.196 overall, but 0.0084 over the 78.9% the console does not clear
tags: oracle,layer-compare,depth,shadow,clear
created: 2026-08-11
updated: 2026-08-11
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

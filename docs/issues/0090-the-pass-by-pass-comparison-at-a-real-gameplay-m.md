---
id: 90
title: The pass-by-pass comparison at a real gameplay moment: our presented frame matches, five copies are never executed
status: resolved
symptom: paired per-pass capture 300 frames past the fade-in: the presented buffer agrees per channel with the console, but our renderer executes 13 of the frame's 19 copy draws
tags: oracle,render,gameplay-scene,resolve,method
created: 2026-08-07
updated: 2026-08-24
---

The first paired per-pass capture that lands on a comparable moment. Both sides
selected by CONTENT and by the same rule -- 300 frames after the first frame
with >= 400 draws (see #89) -- so ours is guest frame 3223 and the console's is
3234, about 0.37 s apart at 30 Hz rather than a whole game moment apart.

Tool: tools/layer_capture.sh + tools/layer_compare.py (instrument I031).

## The presented buffer AGREES

The last copy of the frame, our 0x311000 against the console's 0x1F606000, both
k_8_8_8_8, both read back off the GPU right after the copy:

    ours    R 0.0181  G 0.0279  B 0.0301
    theirs  R 0.0179  G 0.0282  B 0.0292
    10.0% of pixels differ by more than 0.05 in any channel

The side-by-side is the same cell interior with the same tone; the differing
tenth is camera motion over the eleven frames between the two captures. At THIS
moment our renderer is not producing a wrong image.

That also retires, for this moment, the reading that our gameplay render is
black: the black frame is the fade-in and the console is black there too (#73).

## What is NOT executed

    per-resolve snapshots: 13 captured, of 13 resolves this renderer EXECUTED
    and 19 copy draws the frame CONTAINS
    frame resolves not served: 5 from depth (no host depth texture chain yet)

So five of the console's copies have no counterpart here, and the comparison
names them: srcD000 1280x720 f23, srcD000 1280x208 f23, srcD5A0 864x864 f22 x2
-- the scene depth copy and the shadow-map copies -- plus srcC400 1280x208 f32.
Every consumer of a resolved DEPTH buffer therefore reads something we never
wrote.

Conversely we resolve srcC400 1280x720 f32 five times where the console resolves
it twice, which is the predicated-tile model: our surface target already holds
every tile, so each tile's resolve carries the whole surface.

## What this run CANNOT say

The two intermediate k_8_8_8_8 pairs it reports as differing (ours ~0.0025
against the console's 0.8354 and 0.9072, the console's being near-binary masks)
are NOT established as the same pass. The join is (source surface, destination
size, format, ordinal among copies sharing that key), and the console executes
four copies of surface 0x2d0 that we do not -- so the ordinal inside that key
can point at different passes on the two sides. The last two pairs line up
(the front buffer agrees per channel), the first two are unproven. Fixing this
needs a key that does not depend on the two sides executing the same copies:
the draw's position within the frame's pass structure rather than a count.

### Note (2026-08-11)
## CORRECTION: the depth copies were never missing -- the instrument was (2026-08-11)

The headline above ("five of the frame's copies have no counterpart here, and
they are the DEPTH resolves") is WRONG, and it was wrong when it was written.
This renderer has dispatched depth resolves since 2026-07-28 ("Serve the
resolved depth, and the light shafts appear"). Two instrument defects hid that,
and a third stopped the passes pairing even once they were visible.

1. A LOG LINE THAT NEVER STOPPED SAYING "NOT SERVED". `PlanResolves` counted
   EVERY depth copy into `fromDepth` and printed "frame resolves not served: N
   from depth (no host depth texture chain yet)" -- text carried over from
   before the depth chain existed. The SAME log, ~250 lines later, said "frame
   depth resolves: 2 executed, 0 skipped". This entry believed the first line.

2. THE PER-RESOLVE SNAPSHOT SKIPPED DEPTH. `snapshotResolveTarget` was called
   only in the colour branch of the resolve loop; the depth branch `continue`d
   above it. So a depth copy that ran wrote no file, and a pass ABSENT from the
   comparison's input is indistinguishable there from a pass the renderer never
   executed. That is what produced "13 captured, of 13 EXECUTED and 19 the frame
   CONTAINS".

3. THE DEPTH KEY DID NOT MATCH THE CONSOLE'S, for two independent reasons.
   Our source base was the sentinel 0xFFFFFFFF (`srcDFFFFFFFF`) where the
   console names the depth surface (`srcD000`, `srcD5A0`); and our destination
   format was the raw RB_COPY_DEST_INFO.copy_dest_format, which reads 6
   (k_8_8_8_8) for every depth copy of this title, where the console names f23
   and f22. The console is not reading a different register: for a depth copy
   the destination format comes from RB_DEPTH_INFO.depth_format, and Xenia
   implements exactly that (draw_util.cc GetResolveInfo:
   `dest_format = DepthRenderTargetToTextureFormat(rb_depth_info.depth_format)`
   when is_depth, overwriting copy_dest_format with it). kD24S8 -> k_24_8 (22),
   kD24FS8 -> k_24_8_FLOAT (23).

MEASURED AFTER THE FIX, fresh paired capture (ours guest frame 3219, console
3222, same content selector):

    passes both sides resolve: 12          (was 9)
      only ours   (4): srcC400 1280x720 f32 #1..#4
      only theirs (6): srcC2D0 f7 x2, srcC2D0 f32 x2, srcC400 1280x208 f32,
                       srcD000 1280x208 f23
      srcD000 1280x720 f23 #0   -- paired
      srcD5A0  864x864 f22 #0   -- paired
      srcD5A0  864x864 f22 #1   -- paired

So of the five copies this entry called "never served", THREE were executed all
along and now pair, and the other two (srcD000 1280x208, srcC400 1280x208) are
the second tile of a predicated pair, dropped by the tiling collapse by design.
The frame's real coverage is 16 of 18 copy draws, not 13 of 19.

WHAT IS STILL TRUE from the original entry: the presented buffer agrees per
channel, and the two intermediate srcC2D0 pairs still DIFFER. The join for
those is now much stronger -- the console's srcC2D0 copies under the f7 and f32
keys no longer shift the f6 ordinals, and both sides execute exactly four f6
copies of that surface -- so that difference is now a lead rather than an
artefact. It is catalogued separately.

WHAT IS ALSO NEW, and is NOT a fix: RB_DEPTH_INFO.depth_format varies WITHIN the
frame -- the scene depth at EDRAM 0x000 is kD24FS8 and the shadow maps at 0x5a0
are kD24S8 -- while ResolveDepthTo pinned its format selector to float24 for
every copy. The selector is now taken from the register per copy. It changes
nothing observable today and must not be reported as a fix: the depth resolve
shader computes both encodings and DISCARDS them (`(void)depth24`), writing the
decoded float depth because the consumers fetch this destination as a depth
texture. Verified byte-identical output on courtyard.gfr before and after.

Instrument I031 is DISTRUSTED; the repaired tool is I032. Claim C026 is
falsified and replaced by C027.

### Resolution (2026-08-24)
Current-code synchronous rebaseline pairs all 24 resolve handoffs with zero only-native and zero only-oracle passes. All color passes have 0.00% of available pixels over the 0.1 threshold, and all 12 depth passes are value-compared and match. The historic missing-copy and intermediate-difference findings were instrument defects or pre-current renderer results.

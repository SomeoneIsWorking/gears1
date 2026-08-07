---
id: 90
title: The pass-by-pass comparison at a real gameplay moment: our presented frame matches, five copies are never executed
status: open
symptom: paired per-pass capture 300 frames past the fade-in: the presented buffer agrees per channel with the console, but our renderer executes 13 of the frame's 19 copy draws
tags: oracle,render,gameplay-scene,resolve,method
created: 2026-08-07
updated: 2026-08-07
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

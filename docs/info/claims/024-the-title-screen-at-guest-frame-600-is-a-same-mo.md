---
id: C024
kind: claim
status: holds
created: 2026-08-07
tags: 
---

## Claim

The title screen at guest frame 600 is a same-moment cross-emulator comparison, and our guest submits the animated background pass on 0.5% of the frames it should

## Evidence

tools/oracle_lockstep.sh with the shared frame-keyed walk (tools/menu_walk.sh GEARS_WALK_TABLE) and both filmstrips numbered by the guest present counter. Frame 600 is the title screen on both sides with the logo, PRESS START and copyright line aligned. Ours mean RGB 0.1045/0.0533/0.0431 against the oracle's 0.2508/0.0713/0.0499. Same-frame draw-stream diff: 7 (vs,ps) pairs, ~11 draws, bound by the console and not by us; our RAW stream (above every drop site) shows 0 pairs programmed and never prepared, so we do not drop them -- our guest programs all 7 on only 5-12 frames of 1581 (566, 679-681, 753-769, 1249-1262, 1477, 1502) while the oracle draws them on 314-333 frames of 904, contiguously over 571..903.

## What would falsify it

if the seven pairs turn out to be an animation phase rather than a persistent layer -- check by confirming the oracle's span is contiguous and ours is not over a LONGER run than 904/1581 frames; or if our runtime starts submitting them every frame after any change, which would mean the gate was ours and is gone

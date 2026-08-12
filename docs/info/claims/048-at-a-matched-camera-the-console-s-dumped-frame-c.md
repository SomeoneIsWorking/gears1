---
id: C048
kind: claim
status: holds
created: 2026-08-12
tags: oracle,comparison,shadow,mask
depends: tools/camera_pair.sh
---

## Claim

At a matched camera the console's dumped frame carries EIGHT shading draws of the shadow-mask shader where ours carries four -- different numbers of shadow-casting lights -- so draw-to-draw attribution across the two sides is not available even on a paired capture.

## Evidence

scratch/camerapair_ps, PRIM_STATS=2eacd9d94a7dce71 on the same oracle run that produced the camera (guest frame 793, camera matched at 0.26 thresholds): 8 measured draws, fragment invocations 116,988 / 472,102 / 0 / 84,741 / 728 / 32,412 / 0 and one more, total 1,603,393, none dropped past the pool capacity of 256. Our frame has 4 draws of that shader totalling 718,675. A separate oracle run earlier gave 4 draws totalling 806,096, so the count varies between console runs too.

## What would falsify it

a paired capture in which both sides carry the same number of shading draws of this shader, which would make the difference a property of those particular runs rather than of the comparison

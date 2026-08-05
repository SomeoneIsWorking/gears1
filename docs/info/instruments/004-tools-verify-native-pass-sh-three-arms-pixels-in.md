---
id: I004
kind: instrument
status: trusted
created: 2026-08-05
---

## Instrument

tools/verify_native_pass.sh (three arms: pixels, interface, negative control)

## Validated by

Validated in BOTH directions on the same capture, which is the whole point of the second arm. With scene_gamma.frag's image declaration deliberately reverted to texture2D against the renderer's VK_IMAGE_VIEW_TYPE_2D_ARRAY descriptors, the PIXEL arm still reported 2,764,800 of 2,764,800 identical -- a perfect match on a shader that was wrong -- and the new interface arm reported 3 validation warnings and the script exited 1. With the declaration corrected the same command exits 0 with 0 warnings. It also refuses rather than passing when no native pass was substituted, when an arm produces no screenshot, and when both frames are essentially black (observed: it called play_v2.gfr INCONCLUSIVE rather than a match). Its blind spot, stated: it compares one frame per capture, so a pass whose divergence needs different constants than that frame carries will still read as a match.

## Known failure modes

(none recorded yet)

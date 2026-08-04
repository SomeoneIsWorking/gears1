---
id: 53
title: The startup movies never move: the texture cache never evicts a texture the guest overwrote in place
status: resolved
symptom: The pre-menu movie phase shows one stuck near-black image with a faint glow in a corner, flickering between two frames; the intro logos never play
tags: gpu,draw,draw-backend,textures,cache,movie,bink
created: 2026-08-04
updated: 2026-08-04
---

## Symptom

From boot to the title screen the window shows a near-black still with a faint
brighter patch in one corner, alternating between two images. The intro legal
screen and the Epic Games logo animation never appear. Reported from a windowed
run; reproduced headless as reported frames whose mean brightness was one of
exactly two values (2.09 and 0.67 out of 255) for 570 consecutive frames.

## Cause

The guest-texture cache in `runtime/gpu_draw.cpp` is keyed on the TEXTURE FETCH
CONSTANT -- base address, dimensions, format, swizzle. The startup movie player
decodes each Bink frame into the SAME three guest buffers (Y 1280x720 k_8 at
0x880000, U and V 640x360 k_8 at 0x970000/0x9c0000) and re-issues the SAME fetch
constants, so the key never changes while the pixels change every frame. The
cache returned the image built from the first frame forever, and the first frame
of the movie is black.

Nothing was wrong with the movie path itself: the guest's Bink decode, the k_8
linear decode, the plane upload and the guest's own YUV->RGB pixel shader were
all verified correct -- dumping the Y plane (`GEARS_DRAW_TEX_DUMP=1`) gives a
clean Epic Games logo, and a SINGLE-frame capture rendered offline shows the logo
correctly, because a fresh renderer has an empty cache. Only a live run, where
the cache survives from frame to frame, shows the bug. That is why every offline
replay of a captured frame looked right.

## Why the detector had already said no

`GEARS_DRAW_TEXCHECK=1` existed and re-hashed cache hits against their guest
bytes. It was run once, on a captured GAMEPLAY frame: 176 hits, 0 changed. The
conclusion recorded in the code was "the cache does not go stale in this content
and eviction would be speculative work" -- true of that frame, and generalised to
the renderer. The movie phase falsifies it. A measurement on one content type is
not a property of the cache.

## Fix

Eviction, on by default, in the same place the check was:

- The guest bytes behind a cache hit are re-hashed and compared with the hash the
  entry was built from; a mismatch retires the old image and re-uploads.
- The retired image is destroyed after the frame's fence, not on the spot --
  draws already recorded into the frame's command buffer may still reference it.
- The check runs at most ONCE PER FRAME per distinct texture. This is exact, not
  an approximation: the renderer reads guest memory at frame-render time, so
  every binding in a frame sees the same bytes. Per binding it cost 2.3 s on a
  gameplay frame (5094 bindings); per distinct texture it is 130 hashes of
  15.5 MiB total.
- `HashGuestTexture` now mixes eight bytes at a time instead of one, with the
  same total coverage (`tests/test_guest_texture_hash.cpp` flips every byte of a
  span in turn and still passes).
- `GEARS_DRAW_TEXCHECK` is gone: it gated the mechanism that makes the renderer
  correct.

## Verified

Live headless run, reported frames every 10: consecutive movie frames now differ
(mean absolute delta 1.4-58 per frame where it was exactly 0.000 before), and the
frames are the real intro -- the "GAME EXPERIENCE MAY CHANGE DURING ONLINE PLAY"
legal card, then the Epic Games logo animation. The eviction counter reports
"3 distinct textures re-hashed, 1 CHANGED ... evicted and re-uploaded" per movie
frame. The captured Act 1 gameplay frame replays BYTE-IDENTICAL to before the
change (sha256 7c13b92b...), with 130 textures re-hashed and 0 evicted -- so the
fix changes exactly the frames it should.

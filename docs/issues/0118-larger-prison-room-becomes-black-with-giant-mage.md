---
id: 118
title: Larger prison room becomes black with giant magenta wedges during a live walk
status: resolved
symptom: moving from the small Act 1 prison cell toward a larger room turns most of the image black and produces giant bright-magenta diagonal geometry
tags: render,gameplay,temporal,geometry,reported
created: 2026-08-21
updated: 2026-08-21
---

## Root cause

`EVENT_WRITE_SHD` is the title's GPU-retirement fence. The command processor
published its guest-memory write immediately even after live rendering moved to
another host thread. The title therefore learned that prior GPU work was
complete while that thread was still reading the frame's guest vertices,
indices and textures, and could reuse those bytes underneath it. The resulting
mixed uploads poisoned persistent renderer state. That is why a frame captured
after the corruption still looked wrong when rendered synchronously in the live
process, while the exact same 928 MiB `.gfr` rendered coherently in a fresh
offline renderer.

The first boot frame also established persistent EDRAM surface `0x2d0` as
RGBA8, while gameplay later required its mixed-format RGBA16F container. The
cache was keyed only by EDRAM base and never grew its format capacity. That was
a separate real cache defect, but promoting the target did not remove the
larger-room corruption; it is not the root cause of the wedges.

## What was tried / dead ends

- Raw frame size is not the trigger: `chapter45_recovered.gfr` renders 1,742
  draws coherently, and a clean replay renders the exact 5,612-draw failing
  frame coherently.
- Per-frame arena overflow is correlated with entering the room but not causal.
  The arena grew from 17 to 59 MiB and later bad frames reported zero overflow.
- Persistent target format promotion fired in a live control run and repaired
  the target's capacity, but the frame still had only 21,864 non-black pixels
  and the long cyan diagonal. The investigation continued rather than treating
  that secondary bug as the visual fix.

## Resolution

Before storing an `EVENT_WRITE_SHD` value, the command processor now waits for
the renderer's already-existing completion boundary. This implements the event's
actual retirement ordering; it is neither a timed delay nor a cache reset. The
persistent surface cache also accumulates every guest colour format seen for a
base and recreates the Vulkan target only when a wider container is required.

Verified headlessly with the same Lucent-driven input script. Before the fence
fix, the larger-room probe rendered 4,402 draws but only 21,864 non-black pixels
and showed the giant cyan/magenta line. After the fix, guest frame 6,381 rendered
6,809 draws with **zero dropped renderer frames**, 913,652 of 921,600 pixels
non-black, mean RGB `(27.854, 27.546, 18.981)`, and a coherent prison-room view
with Marcus visible. Rotating back and the captured-frame replay remain
coherent.

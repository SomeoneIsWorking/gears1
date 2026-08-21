---
id: 116
title: Depth surface probe copied images without TRANSFER_SRC usage
status: resolved
symptom: GEARS_DRAW_DEPTH_DUMP emitted Vulkan validation errors while still writing plausible depth and stencil files
tags: render,probe,depth,vulkan,validation
created: 2026-08-21
updated: 2026-08-21
---

## Root cause

Depth targets were created with TRANSFER_DST for guest clears but not TRANSFER_SRC, while FrameProbe::DumpDepth transitions them to TRANSFER_SRC_OPTIMAL and copies both aspects to a buffer. The probe was invalid even when its readback looked usable.

## Resolution

DepthTarget image usage now includes TRANSFER_SRC. A headless walk_gameplay.gfr run with GEARS_DRAW_SURFACE_DUMP=650, GEARS_DRAW_DEPTH_DUMP=612 and GEARS_DRAW_VALIDATE=1 took both real probes with zero Vulkan validation errors.

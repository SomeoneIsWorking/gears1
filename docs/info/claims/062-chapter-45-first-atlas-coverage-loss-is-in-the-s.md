---
id: C062
kind: claim
status: holds
created: 2026-08-14
tags: rendering,oracle,shadow,texture
depends: runtime/gpu_draw_textures.cpp#TextureBinder::SelectView, runtime/gpu_draw.cpp#Renderer::RenderFrameImpl
---

## Claim

Chapter-45 first-atlas coverage loss is in the sampled opacity value, not caster geometry or raster coverage: with native guest textures replaced by the existing 1x1 white control, the 10,932- and 8,154-index tiles cover exactly the oracle pixel counts (31,618 and 12,147), while normal native covers 13,491 and 5,884.

## Evidence

scratch/ch45_white_texture_20260814 and scratch/camerapair_chapter45_indexfix_20260814; exact-camera capture, 27-step input self-check, issue #109

## What would falsify it

A same-camera run in which the white-texture control does not reproduce the oracle unique-pixel counts for both same-tile casters, or evidence that GEARS_DRAW_NOTEX changes non-texture raster state

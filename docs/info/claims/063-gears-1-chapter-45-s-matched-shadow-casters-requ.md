---
id: C063
kind: claim
status: holds
created: 2026-08-14
tags: rendering,oracle,texture,shadow
depends: runtime/gpu_draw_texture_decode.cpp#DecodeGuestTexture, runtime/gpu_draw_textures.cpp#TextureUploader::Upload
---

## Claim

Gears 1 chapter-45's matched shadow casters require the guest's authored mip chain: decoding and uploading all declared packed mips makes native sample 0.968719 exactly like the verified Xenia oracle and restores unique-pixel coverage to 31,621 versus 31,618 and 12,147 versus 12,147.

## Evidence

scratch/ch45_full_mips_depth_baseline_20260814, scratch/ch45_full_mips_delta_20260814, scratch/ch45_full_mips_each_20260814, scratch/ch45_delta_probe_20260814/oracle/run.log, and catalog #109

## What would falsify it

A byte-identical cached Gears 1 chapter-45 frame where the pass-preserving sample probe no longer returns 0.968719 on the matched casters, or where either identity-matched caster's unique-pixel coverage departs materially from the oracle after full authored-mip upload

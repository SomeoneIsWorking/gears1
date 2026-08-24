---
id: C081
kind: claim
status: holds
created: 2026-08-24
tags: render,oracle
depends: runtime/gpu_draw_textures.cpp#TextureUploader::ResolveTargetView, tools/layer_compare.py, tools/layer_compare_ranges.py
---

## Claim

Current chapter-45 compatibility-renderer output has 24/24 structurally paired synchronous-oracle resolve handoffs, with all colour rows at 0.00% over the 0.1 threshold and all 12 depth rows matching.

## Evidence

Current-code GEARS_DRAW_RESOLVE_DUMP_EACH replay of scratch/frames/chapter45_recovered.gfr, compared by tools/layer_compare.py against scratch/ch45_sync_full_raw after the forced-white negative control; zero one-sided passes, zero refused passes, 12/12 depth value-compared.

## What would falsify it

A renderer, resolve decoder, tiling/range locator, capture, or synchronous oracle corpus change causes any handoff to become one-sided, refused, or exceed the recorded thresholds.

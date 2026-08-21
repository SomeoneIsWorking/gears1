---
id: 117
title: Cross-sample-count EDRAM preservation needs an explicit representation transfer
status: open
symptom: A future frame that loads existing EDRAM contents immediately after switching one base between native 2X and expanded 1X/4X would read a separately cleared host image
tags: render,msaa,edram,ownership
created: 2026-08-21
updated: 2026-08-21
---

## Scope

Native 2X and expanded 1X/4X targets deliberately have separate Vulkan ownership because their raster sample positions cannot share one attachment. walk_gameplay.gfr starts each representation as a fresh render view; all 16 resolve outputs match the oracle after the split.

## Remaining rule

If a capture switches sample count and the first operation in the new view must preserve prior EDRAM bits, implement an explicit pack/unpack transfer at that transition. Do not make the two views alias memory or reintroduce the quarter-pixel viewport shift. The trigger must be derived from guest load/overwrite semantics, not special-cased by draw number.

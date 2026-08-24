---
id: 131
title: Fatal switch gate reports eight case targets outside their owners
status: resolved
symptom: Gears XenonRecomp generation exits nonzero with eight unreachable switch targets after inline data ranges are declared
tags: recompiler,switch,cfg,data-range
created: 2026-08-24
updated: 2026-08-24
---

## Root cause

The recompiler modeled a function as one contiguous interval discovered by a
static CFG walk that stops at `bctr`. Configured switch labels after an inline
data hole could therefore have no pre-seeded function fragment. Extending an
owner to the maximum target would cross data and absorb unrelated functions;
rejecting the labels left valid computed dispatches unreachable.

## What was tried / dead ends

The older maximum-target extent repair was rejected because it decoded inline
tables as PPC instructions and could silently turn foreign code into the switch
owner. Adding the eight labels as standalone functions was also invalid: they
are case blocks that depend on the owning function's frame and epilogue.

## Resolution

### Resolution (2026-08-24)
Replaced contiguous ownership with disjoint executable blocks and bounded switch-aware CFG discovery. Configured labels seed analysis only inside executable non-data ranges and cannot cross authoritative foreign envelopes. Real Gears regeneration reached 100% with zero generated errors: each of the eight targets has one local dispatch edge and label, none is a standalone function, and no function starts at the inline table words.

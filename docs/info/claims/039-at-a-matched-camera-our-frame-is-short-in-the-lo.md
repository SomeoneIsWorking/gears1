---
id: C039
kind: claim
status: holds
created: 2026-08-12
tags: render,colour,bloom,oracle
depends: tools/front_buffer_percentiles.py, runtime/gpu_draw_resolve.cpp
---

## Claim

At a matched camera our frame is short in the LOW-MIDS and not at the top: median 2.0x and p90 3.5x below the console, p99 18% above it, p99.9 0.85x, max equal. Catalog #62's 'we under-reach at the top' framing is an artefact of comparing unmatched moments.

## Evidence

tools/front_buffer_percentiles.py on scratch/camgate/match/resolve_15 (our front buffer 0x311000) against scratch/vsord/theirs/oracle_f571_copy12 (the console's front buffer 0x1F606000 from the frame our capture was camera-gated to). Green: ours median 0.0118 p90 0.0392 p99 0.3569 p99.9 0.7725 max 1.0000; theirs 0.0235 / 0.1373 / 0.3020 / 0.9059 / 1.0000. Mechanism candidate measured on the same pair: our three srcC5A0 352x182 bloom destinations are 0 of 192192 components non-zero while the console's carry 1.44-1.81% non-zero bytes (byte-level check, no decoder involved).

## What would falsify it

a second camera-matched pair in which our median and p90 land on the console's, or one in which our p99.9/max fall well short -- either would mean this shape is moment-specific rather than the renderer's

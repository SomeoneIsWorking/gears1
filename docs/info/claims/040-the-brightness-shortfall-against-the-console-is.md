---
id: C040
kind: claim
status: holds
created: 2026-08-12
tags: render,colour,bloom,resolve,oracle
depends: runtime/gpu_draw_resolve.cpp, runtime/gpu_draw_resolve_decode.cpp
---

## Claim

The brightness shortfall against the console is present at the FRAME'S FIRST COLOUR RESOLVE, ~3.4x, so it is upstream of the entire post chain; black bloom (#81) and the midtone deficit (#62) are both consequences of it.

## Evidence

tools/front_buffer_percentiles.py at the matched camera (scratch/camgate/match vs oracle frame 571). Scene resolve srcC400 1280x720 f32, our draw 638, vs oracle_f571_copy0: ours max R 0.4196 G 0.3412 B 0.3020; theirs R 1.4453 G 1.2520 B 0.7129. Composite resolves at draws 1064/1092 vs copy6/copy7 repeat it at 0.4549/1.4453 and 0.4549/1.4375. The bright pass thresholds sgt against c255.x=1.0 (disassembled, catalog #81), so the console's input crosses that threshold in R and G and ours crosses it in no channel.

## What would falsify it

a matched-camera pair in which the scene resolve agrees at the top while a later resolve does not -- that would put the loss back inside the post chain. Also: our side is an 8-bit PPM readback that cannot represent >1.0, so if a future dump shows a pile-up at 255 the ratio is a floor rather than a measurement

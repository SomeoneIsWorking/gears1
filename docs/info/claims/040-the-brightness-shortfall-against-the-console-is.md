---
id: C040
kind: claim
status: falsified
created: 2026-08-12
tags: render,colour,bloom,resolve,oracle
depends: runtime/gpu_draw_resolve.cpp, runtime/gpu_draw_resolve_decode.cpp
falsified_on: 2026-08-12
---

## Claim

The brightness shortfall against the console is present at the FRAME'S FIRST COLOUR RESOLVE, ~3.4x, so it is upstream of the entire post chain; black bloom (#81) and the midtone deficit (#62) are both consequences of it.

## Evidence

tools/front_buffer_percentiles.py at the matched camera (scratch/camgate/match vs oracle frame 571). Scene resolve srcC400 1280x720 f32, our draw 638, vs oracle_f571_copy0: ours max R 0.4196 G 0.3412 B 0.3020; theirs R 1.4453 G 1.2520 B 0.7129. Composite resolves at draws 1064/1092 vs copy6/copy7 repeat it at 0.4549/1.4453 and 0.4549/1.4375. The bright pass thresholds sgt against c255.x=1.0 (disassembled, catalog #81), so the console's input crosses that threshold in R and G and ours crosses it in no channel.

## What would falsify it

a matched-camera pair in which the scene resolve agrees at the top while a later resolve does not -- that would put the loss back inside the post chain. Also: our side is an 8-bit PPM readback that cannot represent >1.0, so if a future dump shows a pile-up at 255 the ratio is a floor rather than a measurement

## FALSIFIED 2026-08-12

The camera-gated pair this claim rests on is NOT the same picture, so no per-pixel or per-percentile number taken from it means anything. Measured with a POSITIVE CONTROL, which is what was missing: log-space Pearson correlation of luminance between our front buffer and the console's is 0.073 (linear 0.127), and no vertical flip, horizontal flip or shift up to +/-64 px lifts it above 0.157. The SAME metric on a pair that must agree -- our frame.ppm against our own front-buffer resolve -- scores 0.934 (linear 0.986). So 0.93 is what 'the same picture' looks like through this metric at this quantization, and the cross-side pair scores 0.07. Confirmed independently by banding: our brightest 210 pixels (0.20..0.45, far above the 8-bit floor) sit where the console reads mean 0.0099, and the console's 33 pixels above 1.0 sit where we read 0.0039. Uncorrelated at the BRIGHT end, where quantization cannot explain it. The camera gate matched the view-projection constants to a distance of 3.77 and that evidently does not imply the same rendered scene. Note what SURVIVES: our three bloom resolve destinations are identically zero (0 of 192,192 components) and the console's carry 1.44-1.81% non-zero bytes. A zero-versus-non-zero presence check does not require the two frames to be the same moment, so catalog #81 reproducing live is unaffected.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.

---
id: C046
kind: claim
status: falsified
created: 2026-08-12
tags: oracle,comparison,pairing,instrument
depends: tools/pair_score.py, tools/camera_pair.sh
falsified_on: 2026-08-12
---

## Claim

The correlation ceiling for any cross-console pixel comparison is set by the console's own frame-to-frame change, which is 0.297 in colour and 0.621 in depth at one frame's gap -- not by the pairing method. Our camera-gated pair beats both (0.376 colour, 0.890 depth).

## Evidence

scratch/camerapair/theirs, console against ITSELF -- same emulator, same renderer, no cross-side issue. Mean log-luminance correlation over consecutive dumped frames: gap 1 colour 0.2973 / depth 0.6213 (n=8), gap 2 0.1811 / 0.5263 (n=7), gap 3 0.0883 / 0.4760 (n=6). The zero-motion control (our frame.ppm against our own front-buffer resolve of the same frame) is 0.94, which is why a 0.60 gate looked reasonable and was not. Our camera-gated pair: colour 0.3761, depth 0.8900.

## What would falsify it

a console self-comparison at gap 1 that scores near the zero-motion control, which would mean the ceiling is high and the pairing really is what limits the comparison

## FALSIFIED 2026-08-12

MY OWN NEXT RUN REFUTES IT, WITHIN THE HOUR. I called the console's gap-1 self-correlation (colour 0.297) a CEILING on any cross-console comparison. It is not a ceiling; it is simply the score at ONE DUMP-INTERVAL of separation. A capture that lands temporally CLOSER to a console frame than the neighbouring console frame does scores higher -- and one just did: the camera gate with rotation constrained produced 0.6082 against console frame 790, more than double the 0.297 gap-1 figure and above the 0.60 gate. The gap-1 number remains a useful yardstick for reading a score, and the underlying observation stands (the console's dumped frames are far apart in GUEST time because dumping runs at ~0.8 fps while the guest advances by wall clock). What is withdrawn is the word ceiling and everything I inferred from it.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.

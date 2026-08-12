---
id: C042
kind: claim
status: holds
created: 2026-08-12
tags: oracle,comparison,provenance,instrument
depends: tools/layer_capture.sh, tools/camera_match.py
---

## Claim

Oracle resolve dumps and our camera-gated captures must come from the SAME oracle run; nothing in either artefact records which run it came from, so cross-run pairing is silent.

## Evidence

scratch/camgate/match (11:34:02) was gated against a constants file that scratch/vsord's 11:54 oracle run then OVERWROTE (11:54:43), and its resolve dumps (11:54:19) are from that later run. Comparing them scored a log-luminance correlation of 0.07 where a genuinely matching pair scores 0.93. Both emulators advance the guest by wall-clock delta (#84/#98), so equal frame numbers across two runs are different moments. Neither the .bin filenames nor our .ppm filenames carry a run id.

## What would falsify it

an artefact naming scheme that carries a run id on both sides, after which a cross-run pair is detectable from the filenames alone rather than from timestamps

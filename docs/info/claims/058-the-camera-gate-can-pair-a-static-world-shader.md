---
id: C058
kind: claim
status: holds
created: 2026-08-14
tags: oracle,pairing,camera,shader,instrument
depends: runtime/vd_null_gpu.cpp, tools/camera_pair.sh
---

## Claim

The camera gate can pair a microcode-established static-world c8..c11 view-projection and refuse a route that does not cross it.

## Evidence

Issue #107. Positive scratch/camerapair_worldcam_positive_20260814: VS cb3cec323318973e, CAMERA_CONST_BASE=8, provenance records base 8 and camera digest f323fe656baacd11; match rotation 0.0004/0.005 and relative translation 0.00037/0.013 = 0.08 thresholds. Negative scratch/camerapair_worldcam_probe_20260814: the same shader/layout produced finite distances but came no closer than 32.54 thresholds and the shipping camera gate refused to capture it.

## What would falsify it

A repeat of the positive route failing its camera/provenance gates, the negative route being accepted without a changed path, or corrected microcode analysis showing c8..c11 are not the view-projection used for clip position.

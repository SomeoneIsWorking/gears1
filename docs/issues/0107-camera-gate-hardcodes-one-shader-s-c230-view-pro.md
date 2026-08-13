---
id: 107
title: Camera gate hardcodes one shader's c230 view-projection layout
status: resolved
symptom: A qualifying oracle scene dumps 8/8 frames but camera_pair refuses because the usual skinned vertex shader never binds
tags: oracle,pairing,camera,shader,tooling
created: 2026-08-14
updated: 2026-08-14
---

Root cause: camera_pair selected shader f3e9368c1bb68ecc and runtime/vd_null_gpu.cpp always read c230..c233. The later continuous-movement scene did not bind that shader, though its dominant static-world VS cb3cec323318973e carries the view-projection at c8..c11: its microcode first transforms position through local c0..c3, then multiplies the result through c8..c11 into clip space. The fix adds explicit `CAMERA_CONST_BASE` (default 230), validates that all four rows exist and the base fits c0..c255, passes it into the runtime gate, and records it in provenance. The base is selected from shader analysis, never by fitting scores.

Positive discriminator: scratch/camerapair_worldcam_positive_20260814 with cb3cec323318973e/c8 matched at 0.08 thresholds after 524 held frames, passed UI/provenance, and scored 0.9458 against oracle f1121—better than the oracle's +1-frame self score 0.7844. first_divergence conservatively priced it at one frame and found all 12 decodable resolves at or above their own yardsticks. Negative discriminator: scratch/camerapair_worldcam_probe_20260814 found the same shader and finite c8..c11 matrices on a later collision-diverged route, but never came closer than 32.54 thresholds and captured nothing.

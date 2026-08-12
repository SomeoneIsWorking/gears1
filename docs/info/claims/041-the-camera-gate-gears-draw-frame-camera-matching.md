---
id: C041
kind: claim
status: falsified
created: 2026-08-12
tags: oracle,camera,comparison,instrument
depends: runtime/vd_null_gpu.cpp, tools/camera_match.py
falsified_on: 2026-08-12
---

## Claim

The camera gate (GEARS_DRAW_FRAME_CAMERA) matching the guest view-projection to a distance of 3.77 does NOT deliver the same rendered scene as the console frame the constants came from.

## Evidence

scratch/camgate/match vs oracle frame 571. Log-luminance correlation of the front buffers 0.073 as given, 0.157 best over vertical/horizontal flips and all shifts to +/-64px; the same metric on our frame.ppm vs our own front-buffer resolve scores 0.934. Bright-end banding agrees where quantization cannot explain it: our brightest 210 px (0.20..0.45) sit where the console reads mean 0.0099, and the console's 33 px above 1.0 sit where we read 0.0039.

## What would falsify it

a camera-gated pair at a tighter threshold that PASSES tools/front_buffer_percentiles.py's same-picture gate -- that would mean 3.77 was merely too loose rather than the constants being the wrong discriminator

## FALSIFIED 2026-08-12

WRONG, AND IT BLAMED A WORKING INSTRUMENT FOR MY OWN ERROR. The 0.07 correlation is not the camera gate failing; it is me comparing our capture against a DIFFERENT ORACLE RUN. Provenance, from the file timestamps: scratch/camgate/match/frame.ppm is 11:34:02 and was gated against theirs_vs_consts_294.txt AS IT EXISTED THEN; scratch/vsord/theirs/oracle_f571_copy12*.bin is 11:54:19 and theirs_vs_consts_294.txt was itself overwritten at 11:54:43 by that later run (it now holds c0..c7 of a different shader, not the c230..c233 the gate parses -- which is how the substitution became visible). Both emulators advance the guest by wall-clock delta (#84, #98), so frame 571 of one oracle run is not the same moment as frame 571 of another. I paired the artefacts by DIRECTORY rather than by RUN. AND THE GATE'S KEY IS CORRECT, checked statically from the microcode rather than assumed: vs f3e9368c1bb68ecc references exactly c0..c4, c230..c233 and c255, and its position export is 'mad oPos, r2.wwww, c233, r1.zxyw' -- so c230..c233 is the view-projection the gate takes it for. The gate also BEHAVED CORRECTLY end to end: it refused to let the mismatched pair be reported, which is what surfaced this at all.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.

---
id: I048
kind: instrument
status: trusted
created: 2026-08-12
---

## Instrument

tools/depth_scale_check.py: compares our float depth dump against the console's decoded depth and flags SIMPLE-FRACTION scale errors that correlation is structurally blind to

## Validated by

Driven against both real classes on real captures. PRE-FIX (scratch/maskstencil/depth_after_diag1072.npy): median ratio 2.000057 over 655,360 pixels, 98.11% within 1% of exactly 2.0 -> exit 1, SCALE ERROR. POST-FIX (scratch/depthfix/depth_after_diag1071.npy): median 0.999986, mean absolute difference 0.477% of the console mean -> exit 0, SCALES AGREE. Selftest drives 2x/0.5x (must flag) and 1.0x/1.31x/same-scale-plus-noise (must not). Refusals exit 3, distinct from a finding's 1, so 'nothing was compared' cannot be recorded as a pass. LIMITATION stated in the file: this is NOT first_divergence.py's scale column, which compares RESOLVED surfaces -- the scene depth resolve reads 1.0019x both before and after the fix because the corruption lived between resolves. Depends: tools/depth_scale_check.py, runtime/gpu_draw_xlate.cpp

## Known failure modes

(none recorded yet)

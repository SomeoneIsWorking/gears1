---
id: I005
kind: instrument
status: trusted
created: 2026-08-05
---

## Instrument

tools/ucode_reduce.py (symbolic register-file reduction of a Xenos shader)

## Validated by

Validated by REPRODUCING a reduction already proved correct downstream: run on ps_1f1a3f779667a02a it derives the same lightmap terms the shipped runtime/shaders/base_pass_lightmap.frag was written from (c3.x * tex(tf4).x componentwise, c5.x * tex(tf6).x, and oC0.w = interpolator2.w), and that shader is bit-exact against the translated shader on two captures. --selftest runs five cases including two it must get NEGATIVE on: a synthetic pair of rotations that compose to the identity must leave oC0.x depending on r1.x and NOT on r1.y or r1.z, and a shader containing  must be REFUSED rather than reduced. The self-test caught a wrong expectation of my own on first run -- a synthetic rotation I asserted would cancel does not -- which is the only reason to trust the case that does. Blind spots, printed with every run: no control flow, no predication, no memexport, and none of the runtime's fetch or output epilogue (texture sign decode, integer scale, exponent bias, gamma encode).

## Known failure modes

(none recorded yet)

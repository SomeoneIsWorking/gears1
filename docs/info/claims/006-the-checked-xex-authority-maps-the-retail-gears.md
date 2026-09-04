---
id: C006
kind: claim
status: holds
created: 2026-08-24
tags: xex,provisioning
depends: tools/title_identity.py, config/titles/gears1.toml, ../../shared/x360port/include/x360port/module_contract.hpp, ../../shared/x360port/include/x360port/validation.hpp
reconfirmed: 2026-08-24
verified_at: 2026-08-24 20:58:29
---

## Claim

The checked XEX authority maps the retail Gears executable to a 13,500,416-byte normalized image with SHA-256 f61cc78e4057bc68a2c65386a0341f6d26a7add3dfd9918007a455750ec6ed5c, 17 sections, and 236 ordered logical imports.

## Evidence

Historical ASan/UBSan checked-inspector evidence matched the real normalized
image byte-for-byte and passed malformed-stage controls. The parser executable
is retired; the resulting authenticated-module and typed-import invariants now
live in `x360port_validation`, while the exact identity remains in the Gears
profile.

## What would falsify it

A fresh run on the same exact default.xex changes the normalized digest/size/counts, fails sanitizers, or accepts one of the malformed-stage controls.

## Re-confirmed 2026-08-24

Historical re-verification at predecessor commit `a841864` passed sanitizer
CTest 9/9; the exact retail run remained byte-identical at SHA-256
`f61cc78e4057bc68a2c65386a0341f6d26a7add3dfd9918007a455750ec6ed5c`
with 17 sections and 236 imports.

## Re-confirmed 2026-08-24

Confirmed after parent commit ae7df48: the 16/16 title-identity tests and real checked-loader run still bind the selected XEX and normalized image to the recorded SHA, size, 17 sections, and 236 imports.

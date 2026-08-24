---
id: C006
kind: claim
status: holds
created: 2026-08-24
tags: xex,provisioning
depends: extern/XenonRecomp/XenonUtils/xex.cpp, extern/XenonRecomp/XenonUtils/image.cpp, extern/XenonRecomp/XexInspect/inspect.cpp, tools/title_identity.py
reconfirmed: 2026-08-24
verified_at: 2026-08-24 20:57:49
---

## Claim

The checked XEX authority maps the retail Gears executable to a 13,500,416-byte normalized image with SHA-256 f61cc78e4057bc68a2c65386a0341f6d26a7add3dfd9918007a455750ec6ed5c, 17 sections, and 236 ordered logical imports.

## Evidence

ASan/UBSan xex-inspect real-input run matched scratch/raw/gears_image.bin byte-for-byte; aggregate XenonRecomp CTest passed 9/9 including malformed-stage XexInspectTests and consolidated Clang quality.

## What would falsify it

A fresh run on the same exact default.xex changes the normalized digest/size/counts, fails sanitizers, or accepts one of the malformed-stage controls.

## Re-confirmed 2026-08-24

Re-verified after XenonRecomp commit a841864: sanitizer CTest 9/9 passed and the exact retail run remained byte-identical at SHA-256 f61cc78e4057bc68a2c65386a0341f6d26a7add3dfd9918007a455750ec6ed5c with 17 sections and 236 imports.

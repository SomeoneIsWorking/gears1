---
id: 132
title: Metadata-valid malformed XEX reaches unchecked loader
status: resolved
symptom: xex-inspect can read or write out of bounds after its execution-metadata precheck accepts malformed container offsets
tags: xex,loader,security,provisioning
created: 2026-08-24
updated: 2026-08-24
---

## Root cause

`InspectXex` validated only the execution-info optional header, then passed the
same container to the legacy pointer-based loader. That loader trusted security,
file-format, compression, PE, section, import, and thunk extents independently,
so passing the shallow precheck said nothing about the safety of the later
reads and writes. Import reconstruction also searched globally by ordinal,
discarding the ordered record/thunk identity encoded by the container.

## What was tried / dead ends

A separate inspector-side bounds precheck was rejected. It would duplicate only
part of the loader grammar and could drift while XenonAnalyse and XenonRecomp
continued to call the unchecked authority.

## Resolution

### Resolution (2026-08-24)
Replaced the split precheck plus unchecked loader with one transactional TryLoadXex authority. Every container, decryption, compression, PE, section, import, record, and thunk extent is checked; ordered adjacent imports refuse ambiguity. ASan/UBSan malformed-stage tests pass, and the retail XEX produces the byte-identical 13,500,416-byte normalized image (SHA-256 f61cc78e4057bc68a2c65386a0341f6d26a7add3dfd9918007a455750ec6ed5c), 17 sections, and 236 imports.

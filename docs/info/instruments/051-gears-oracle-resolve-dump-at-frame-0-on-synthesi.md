---
id: I051
kind: instrument
status: DISTRUSTED
created: 2026-08-14
distrusted_on: 2026-08-20
---

## Instrument

GEARS_ORACLE_RESOLVE_DUMP_AT_FRAME=0 on synthesized one-frame Xenia traces

## Validated by

Before the selector fix frame 0 was treated as disabled and zero raw files were possible; after separating selection from index, the chapter-45 trace wrote all 23 copy destinations from copy 0 through copy 22.

## Known failure modes

(none recorded yet)

## DISTRUSTED 2026-08-20

Emitting all expected raw files validates selection and plumbing, not their pixel content. On the synthesized chapter-45 target, a forced-white shader with depth/stencil disabled produced 147,870 fragment invocations yet the selected raw resolve stayed zero, so zero content from this path is not a trustworthy negative.

> Every result this instrument produced is suspect until it is re-validated.

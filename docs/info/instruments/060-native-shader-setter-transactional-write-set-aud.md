---
id: I060
kind: instrument
status: trusted
created: 2026-08-27
---

## Instrument

native shader-setter transactional write-set audit

## Validated by

test_shader_setter_state forces a comparator mismatch at byte 2; live audit then matched 240/240 eligible setter calls and would abort on the first owned-byte or callee-saved-state difference

## Known failure modes

(none recorded yet)

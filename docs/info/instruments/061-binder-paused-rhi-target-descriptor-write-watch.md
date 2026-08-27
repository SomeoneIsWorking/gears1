---
id: I061
kind: instrument
status: trusted
created: 2026-08-28
---

## Instrument

`GEARS_NATIVE_RHI_OBSERVE=1 GEARS_WATCH_RHI_TARGET_DESCRIPTOR=1` binder-paused slot-zero descriptor write attribution

## Validated by

The first live arm produced the required positive by catching the known color-target binder at host RIP `0xE88B93`. The focused guest-write-watch test proves exact-target, same-page-other-address, paused-write, resumed-write, and sample-limit outcomes. The title adapter then pauses page protection across the known binder super-call and resumes it afterward; the corrected live arm caught a different writer at RIP `0xE837CE`, which resolves to retained guest `0x82229B28` and whose store instruction targets device `+0x2804`.

## Known failure modes

The log prints both full host RIP and module-relative address. The captured ET_EXEC build's `addr2line` expects the full ELF virtual address (`0xE837CE`); feeding it the module-relative value (`0xA837CE`) resolves an unrelated symbol. A relink may move both values, and a PIE build may instead require the module-relative form, so preserve and label both from the same run.

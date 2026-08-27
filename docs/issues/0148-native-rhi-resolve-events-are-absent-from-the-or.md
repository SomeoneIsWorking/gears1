---
id: 148
title: Native RHI resolve events are absent from the ordered semantic stream
status: resolved
symptom: Gears 1 logical resolve calls emit kCopy PM4 but the native RHI stream records only bindings, draws, and present, so a native engine cannot own resolve ordering or resource handoff
tags: performance,native-rhi,resolve,re,gears1
created: 2026-08-28
updated: 2026-08-28
---

## Root cause

The title-neutral stream had no resolve operation because the Gears 1 logical
resolve owner had not been grounded. The compatibility renderer could execute
the resulting kCopy register state, but there was no title-facing semantic
record tying source target identity, destination texture storage, and the
rectangle-list copy packet together in the order the RHI submitted them.

Selective inspection of the locally generated retained body grounded
`0x82235528`. It selects one of four colour targets or the depth target from the
low three flag bits, reads the six-word destination texture descriptor at
object `+0x1C`, derives the destination physical address/pitch/remaining height,
then emits a three-element rectangle-list `DRAW_INDX` or `DRAW_INDX_2` with
auto-index source. The retained body remains compiled and callable.

## What was tried / dead ends

The first live comparison rejected every expanded-format destination even
though the retained device shadow and packet were internally consistent. The
semantic decoder was returning an invalid destination because the shared
`ColorFormatBytesPerPixel` authority omitted formats 27, 28, and 29. Those are
the 16-, 32-, and 64-bit EXPAND aliases of the already-supported formats 30,
31, and 32. Treating the symptom as an address-formula exception would have
hidden the actual shared table defect.

## Resolution

`rhi_resolve.*` independently decodes the call into a title-neutral semantic
resolve. A strong observer around `sub_82235528` snapshots its inputs,
super-calls `__imp__sub_82235528`, bounds packet inspection to only the command
span emitted by that call, and compares source identity, destination
address/pitch/height, draw opcode, rectangle primitive, auto-index source, and
element count. The ordered stream now stores resolve beside binding, draw, and
present events.

The shared colour-format byte-size table now includes all three EXPAND aliases.
Focused tests drive the known `0x0BA40000` destination at row 512 to exact
physical address `0x0BCC0000`, expanded format sizes, colour/depth selection,
invalid input, packet presence, and an out-of-span negative. A real headless
run through frame 540 observed 540 resolves and matched all 540 retained
packets/state records, with zero missing observations and zero mismatches.

### Resolution (2026-08-28)
Grounded 0x82235528, added the ordered semantic resolve event and bounded retained-packet comparison, fixed missing EXPAND-format byte sizes in the shared format authority, and matched 540/540 live headless resolves with zero missing or mismatched observations.

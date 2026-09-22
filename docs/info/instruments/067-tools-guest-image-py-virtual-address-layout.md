---
id: I067
kind: instrument
status: trusted
created: 2026-09-22
---

## Instrument

tools/guest_image.py virtual-address-indexed guest image loader, used by
ppcdis.py, find_addr_refs.py, abstract_vtables.py, and shader_extract.py

## Validated by

Run against both classes. Positive: the mapped image
(`x360-xex-inspect --mapped-image-out`) disassembles `0x825F7B40` as the
audio-mix prologue and `0x82233668` as the ResourceAddRef prologue, both
matching the bytes the runtime reports through `ReadMappedGuestMemory`.
Negative: the normalized image is refused by name with its measured length and
the PE `SizeOfImage` it contradicts, and a missing image is refused with the
exact command that builds it; both exit 2 rather than printing a decode.

## Known failure modes

The discriminator is the PE `SizeOfImage` against the file length, so it
catches the normalized layout only because Gears 1 has section gaps. An image
whose sections happen to be gapless would pass the check while carrying either
layout, because for that image the two layouts are identical.

Before this loader existed the tools read the normalized image with
`--base 0x82000000` and silently decoded a different, plausible-looking
function for every address at or above `.text`, displaced by the 0x4E00
alignment gap. That is how `0x825F2D40` was recorded for an operation whose
real address is `0x825F7B40`.

---
id: 124
title: Gears v374 package compression was misclassified as LZX
status: resolved
symptom: Entry.xxx package decompression failed through every attempted LZX backend despite a documented LZX assumption
tags: native-ue3,package,lzo,lzx,source-lineage
created: 2026-08-22
updated: 2026-08-22
---

## Symptom

The native package milestone was documented and initially implemented as Xbox
360 LZX because the title is a Xenon build and the outer UE3 blocks are 128
KiB. Every libmspack LZX window attempt failed against `Entry.xxx`.

## Root cause

The platform inference overrode direct package evidence. In this Gears v374
package dialect, compression flag 2 selects LZO. The real stream at offset
0x71 is a UE3 nested compression container with three LZO payloads:

- 0x7016 -> 0x20000
- 0x07E2 -> 0x20000
- 0x0417 -> 0x05C78

The production `gears::ue3::decompressLzo` seam decodes all three blocks,
accounts for the exact 0x7C0F compressed payload and 0x45C78 logical bytes,
and yields the first serialized name `ArrayProperty`.

## Resolution evidence

The headless `ue3_host_lzo_entry` CTest exercises the production seam against
the external Gears asset. `ue3_host_lzo_arguments` adds a synthetic positive
round trip, invalid-argument cases, and a deliberately corrupted compressed
stream that the decoder rejects. Generated `UnMisc.cpp` routes
`COMPRESS_LZO` to this host seam on Linux while leaving the unavailable bundled
LZOPro path disabled.

## Dead end

Do not derive a UE3 package codec from the console platform, the outer chunk
size, OpenBL2, or an XEX decompression path. Identify it by an exact successful
decode with declared-size and content checks plus a negative control. OpenBL2
is incomplete and is not an oracle for Gears.

---
id: 172
title: Native audio mix is unreached on the system session
status: resolved
symptom: the product installs the audio-mix override, but a 150 s run through the menus and Act 1's opening scene records 0 calls
tags: audio,native-override,x360port
state_items: S004,S009
created: 2026-09-22
updated: 2026-09-23
---

## Cause

The override was bound at `0x825F7B40`, an address read from a copy of the
image that `x360port::MapPeImage` had re-laid by section VirtualAddress. Xenia's
loader, like the console, leaves each section at its raw offset, and Gears 1's
`.text` sits 0x4E00 below the VirtualAddress its header claims. In the image the
product runs, `0x825F7B40` is in the middle of another function, so no call
ever targeted it. The earlier caller analysis (two `bl` sites near
`0x825F712C`, no direct callers) was done in the same wrong copy and is void.

## Resolution

`x360port::DescribeLoadedPeImage` replaces the relayout, the inspector writes
only the loaded image, and `tools/guest_image.py` refuses a re-laid copy
(instrument I067). The mix is bound at `0x825F2D40`; `test_gears1_real_leaf`
re-qualifies it there against the original body. A 200 s headless gameplay walk
records 5,531,195 native-override calls (about 47,000 per second) and plays
into Act 1 at 118-119 presents/s.

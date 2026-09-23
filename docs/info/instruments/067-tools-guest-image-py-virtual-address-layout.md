---
id: I067
kind: instrument
status: trusted
created: 2026-09-22
updated: 2026-09-23
---

## Instrument

tools/guest_image.py loaded-image loader, used by ppcdis.py,
find_addr_refs.py, abstract_vtables.py, and shader_extract.py

## Validated by

Run against both classes, with a discriminator independent of the tool's own
layout choice. Positive: the image `x360-xex-inspect --image-out` writes (the
basefile as the XEX loader leaves it) decodes `0x825F2D40` and `0x8222E868`
as function prologues, and `test_gears1_real_leaf` executes both bodies at
those addresses on the image Xenia's own loader produces. The retained draw,
shader-setter and flush entries `0x8222CFF8`, `0x82222808`, `0x822346A8`
decode as prologues too. Negative: a copy re-laid by section VirtualAddress
(the real one, and a synthetic one in `tests/test_guest_image.py`) is refused
by name with the first displaced section, and a missing image is refused with
the command that builds it.

## Known failure modes

The refusal keys on a file exactly `SizeOfImage` long with a section whose
VirtualAddress differs from its raw offset. An image whose sections all sit at
their VirtualAddress has one layout, so nothing is lost there.

The first version of this instrument had the layouts backwards: it required the
re-laid copy and refused the loaded one. Its validation was circular, since
both the tool and the runtime image it was checked against came from the same
wrong `x360port::MapPeImage` relayout. That moved the audio mix to
`0x825F7B40` and the resource AddRef leaf to `0x82233668`, both 0x4E00 above
their real addresses, and left the product's audio override installed mid
function. Every address recorded through it was re-checked on 2026-09-23;
addresses recorded before it were already in the loaded layout.

#!/usr/bin/env python3
"""Load the Gears guest image that reverse-engineering addresses refer to.

Guest addresses recovered from Ghidra, traces, and the runtime are addresses in
the image as the XEX loader leaves it at the image base: the decompressed
basefile, copied flat. ``x360-xex-inspect --image-out`` writes exactly that.

The loader does NOT move a section to its PE ``VirtualAddress``. Gears 1's
``.text`` claims ``VirtualAddress`` 0x170000 but sits at raw offset 0x16B200,
0x4E00 lower, and later sections sit further still from their claimed
addresses. A copy re-laid by ``VirtualAddress`` therefore decodes a different,
plausible-looking function at every address at or above ``.text``. That
re-laid copy was once treated as authoritative and moved two recorded
addresses by 0x4E00; the runtime disproved it (see instrument I067). This
loader refuses it by name.

The discriminator is exact for any image in which a section's
``VirtualAddress`` differs from its raw offset: the re-laid copy is exactly
``SizeOfImage`` long with such a section, which no loader produces. An image
whose sections all sit at their ``VirtualAddress`` has one layout, and both
copies are identical.
"""

from __future__ import annotations

import struct
from pathlib import Path

from title_identity import XEX_INSPECT_DEFAULT, XEX_INSPECT_ENV

DEFAULT_IMAGE = Path("scratch/raw/gears_image.bin")
DEFAULT_BASE = 0x82000000
# tools/title_identity.py owns where the inspector is found, including the
# XEX_INSPECT override, so the build command is not spelled a second time here.
BUILD_COMMAND = (
    f"{XEX_INSPECT_DEFAULT} <default.xex> --image-out {DEFAULT_IMAGE} "
    f"(override the tool path with ${XEX_INSPECT_ENV})"
)


class GuestImageError(Exception):
    """The image is missing, unreadable, or not the loaded layout."""


def _pe_geometry(data: bytes) -> tuple[int, list[tuple[int, int]]]:
    """Return SizeOfImage and each section's (VirtualAddress, raw offset)."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise GuestImageError("image does not start with a PE DOS header")
    (pe_offset,) = struct.unpack_from("<I", data, 0x3C)
    if len(data) < pe_offset + 4 + 20 or data[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise GuestImageError("image has no valid PE signature")
    (section_count,) = struct.unpack_from("<H", data, pe_offset + 4 + 2)
    (optional_size,) = struct.unpack_from("<H", data, pe_offset + 4 + 16)
    optional = pe_offset + 4 + 20
    if optional_size < 224 or len(data) < optional + optional_size:
        raise GuestImageError("image has no valid PE32 optional header")
    (size_of_image,) = struct.unpack_from("<I", data, optional + 56)
    table = optional + optional_size
    if len(data) < table + 40 * section_count:
        raise GuestImageError("image section table is truncated")
    sections = [
        struct.unpack_from("<II", data, table + 40 * index + 12)[0:1]
        + struct.unpack_from("<I", data, table + 40 * index + 20)
        for index in range(section_count)
    ]
    return size_of_image, sections


def load_guest_image(path: str | Path = DEFAULT_IMAGE) -> bytes:
    """Return the loaded image, indexed by guest address, or refuse with the reason.

    Refuses a missing file and refuses the re-laid layout by name, so a caller
    can never silently disassemble the wrong bytes.
    """
    image_path = Path(path)
    if not image_path.is_file():
        raise GuestImageError(
            f"guest image {image_path} does not exist; build it with: {BUILD_COMMAND}"
        )
    data = image_path.read_bytes()
    size_of_image, sections = _pe_geometry(data)
    displaced = [(va, raw) for va, raw in sections if va != raw]
    if len(data) == size_of_image and displaced:
        va, raw = displaced[0]
        raise GuestImageError(
            f"guest image {image_path} is SizeOfImage={size_of_image} bytes with its "
            f"sections moved to their PE VirtualAddress (the first moved section "
            f"claims {va:#x} but its bytes belong at raw offset {raw:#x}); no loader "
            f"lays an image out this way. Rebuild the loaded image with: {BUILD_COMMAND}"
        )
    return data


def read_range(data: bytes, base: int, start: int, end: int) -> bytes:
    """Slice ``[start, end)`` by virtual address, refusing an out-of-range span."""
    if end <= start:
        raise GuestImageError(f"empty or inverted range {start:#x}..{end:#x}")
    begin = start - base
    stop = end - base
    if begin < 0 or stop > len(data):
        raise GuestImageError(
            f"range {start:#x}..{end:#x} is outside the image mapped at {base:#x} "
            f"with {len(data)} bytes"
        )
    return data[begin:stop]

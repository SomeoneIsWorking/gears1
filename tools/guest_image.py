#!/usr/bin/env python3
"""Load the Gears guest image that reverse-engineering addresses refer to.

Guest addresses recovered from Ghidra, traces, and the runtime are *virtual*
addresses. Two different byte layouts exist for the same executable and they
are not interchangeable:

* the **normalized** XEX image, whose sections sit at their file raw offsets,
  written by ``x360-xex-inspect --image-out``; and
* the **mapped** image, whose sections sit at their virtual addresses, written
  by ``x360-xex-inspect --mapped-image-out``.

Gears 1 has a 0x4E00 alignment gap in front of ``.text``, so reading the
normalized image as if it were virtual-address-indexed silently yields a
different, still-plausible-looking function for every address at or above
``.text``. That mistake produced a wrong recorded address for the audio-mix
operation, so this loader refuses the wrong layout instead of decoding it.

The discriminator is exact: the mapped image's length equals the PE optional
header's ``SizeOfImage``, while the normalized image is shorter by the section
gaps.
"""

from __future__ import annotations

import struct
from pathlib import Path

from title_identity import XEX_INSPECT_DEFAULT, XEX_INSPECT_ENV

DEFAULT_IMAGE = Path("scratch/raw/gears_mapped.bin")
DEFAULT_BASE = 0x82000000
# tools/title_identity.py owns where the inspector is found, including the
# XEX_INSPECT override, so the build command is not spelled a second time here.
BUILD_COMMAND = (
    f"{XEX_INSPECT_DEFAULT} <default.xex> --mapped-image-out {DEFAULT_IMAGE} "
    f"(override the tool path with ${XEX_INSPECT_ENV})"
)


class GuestImageError(Exception):
    """The image is missing, unreadable, or not virtual-address-indexed."""


def _size_of_image(data: bytes) -> int:
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise GuestImageError("image does not start with a PE DOS header")
    (pe_offset,) = struct.unpack_from("<I", data, 0x3C)
    if len(data) < pe_offset + 4 + 20 or data[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise GuestImageError("image has no valid PE signature")
    (optional_size,) = struct.unpack_from("<H", data, pe_offset + 4 + 16)
    optional = pe_offset + 4 + 20
    if optional_size < 224 or len(data) < optional + optional_size:
        raise GuestImageError("image has no valid PE32 optional header")
    (size_of_image,) = struct.unpack_from("<I", data, optional + 56)
    return size_of_image


def load_mapped_image(path: str | Path = DEFAULT_IMAGE) -> bytes:
    """Return the virtual-address-indexed image, or refuse with the reason.

    Refuses a missing file and refuses the normalized layout by name, so a
    caller can never silently disassemble the wrong bytes.
    """
    image_path = Path(path)
    if not image_path.is_file():
        raise GuestImageError(
            f"guest image {image_path} does not exist; build it with: {BUILD_COMMAND}"
        )
    data = image_path.read_bytes()
    size_of_image = _size_of_image(data)
    if len(data) != size_of_image:
        raise GuestImageError(
            f"guest image {image_path} is {len(data)} bytes but its PE header declares "
            f"SizeOfImage={size_of_image}; this is the normalized (raw-offset) layout, "
            f"whose sections are displaced from their virtual addresses. Rebuild the "
            f"virtual-address-indexed image with: {BUILD_COMMAND}"
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

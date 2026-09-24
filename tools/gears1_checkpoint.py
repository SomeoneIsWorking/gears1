"""The campaign checkpoint Gears 1 last saved, read from the product's storage.

The title saves ``default_checkpoint.sav`` in its content directory. Its file
begins with a 16-byte header, then three strings, each a big-endian int32
length (counting its NUL) and NUL-terminated ASCII: the persistent map, the
streaming level that holds the checkpoint, and the checkpoint's object path in
that level (``<level>.TheWorld.PersistentLevel.WarCheckpoint_<n>``).
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

TITLE_ID = "4D5307D5"
SAVE_DIRECTORY = "default_checkpoint.sav"
HEADER_BYTES = 16
MAX_STRING_BYTES = 256


class CheckpointError(RuntimeError):
    """The storage holds no readable checkpoint save."""


@dataclass(frozen=True)
class Checkpoint:
    map: str
    level: str
    name: str


def parse_checkpoint(data: bytes) -> Checkpoint:
    offset = HEADER_BYTES
    strings = []
    for field in ("map", "level", "checkpoint"):
        if offset + 4 > len(data):
            raise CheckpointError(f"the save ends before its {field} name")
        (length,) = struct.unpack_from(">i", data, offset)
        offset += 4
        if not 1 <= length <= MAX_STRING_BYTES or offset + length > len(data):
            raise CheckpointError(f"the save's {field} name has length {length}")
        text = data[offset:offset + length]
        offset += length
        if text[-1] != 0 or 0 in text[:-1] or not text[:-1].isascii():
            raise CheckpointError(f"the save's {field} name is not a NUL-terminated ASCII string")
        strings.append(text[:-1].decode("ascii"))
    map_name, level, path = strings
    prefix = f"{level}.TheWorld.PersistentLevel."
    if not path.startswith(prefix) or "." in path[len(prefix):]:
        raise CheckpointError(f"checkpoint {path!r} is not an object of level {level!r}")
    return Checkpoint(map_name, level, path[len(prefix):])


def read_checkpoint(storage_root: Path) -> Checkpoint | None:
    """The saved checkpoint, or None before the title has saved one."""

    saves = sorted(storage_root.glob(f"content/*/{TITLE_ID}/*/{SAVE_DIRECTORY}/*"))
    if not saves:
        return None
    if len(saves) != 1:
        raise CheckpointError(f"{len(saves)} checkpoint saves under {storage_root}, not one")
    return parse_checkpoint(saves[0].read_bytes())

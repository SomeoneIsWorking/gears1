"""The Gears 1 checkpoint save's names, parsed from synthetic saves."""

from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from tools.gears1_checkpoint import (  # noqa: E402
    Checkpoint,
    CheckpointError,
    parse_checkpoint,
    read_checkpoint,
)


def fstring(text: str) -> bytes:
    encoded = text.encode("ascii") + b"\0"
    return struct.pack(">i", len(encoded)) + encoded


def save(level: str = "SP_Prison_S04_Scripting", checkpoint: str = "WarCheckpoint_1") -> bytes:
    return (struct.pack(">4I", 2, 0x25, 0, 0) + fstring("sp_prison_p") + fstring(level)
            + fstring(f"{level}.TheWorld.PersistentLevel.{checkpoint}") + b"\0\0\0\0$Warfare")


class ParseTest(unittest.TestCase):
    def test_reads_the_three_names(self) -> None:
        self.assertEqual(parse_checkpoint(save()),
                         Checkpoint("sp_prison_p", "SP_Prison_S04_Scripting", "WarCheckpoint_1"))

    def test_truncated_save_refuses(self) -> None:
        with self.assertRaisesRegex(CheckpointError, "ends before its checkpoint name"):
            parse_checkpoint(save()[:0x3C])

    def test_absurd_length_refuses(self) -> None:
        data = bytearray(save())
        data[16:20] = struct.pack(">i", 100000)
        with self.assertRaisesRegex(CheckpointError, "map name has length 100000"):
            parse_checkpoint(bytes(data))

    def test_unterminated_string_refuses(self) -> None:
        data = bytearray(save())
        data[16 + 4 + 11] = ord("x")
        with self.assertRaisesRegex(CheckpointError, "map name is not a NUL-terminated"):
            parse_checkpoint(bytes(data))

    def test_checkpoint_of_another_level_refuses(self) -> None:
        data = (struct.pack(">4I", 2, 0x25, 0, 0) + fstring("sp_prison_p") + fstring("SP_A")
                + fstring("SP_B.TheWorld.PersistentLevel.WarCheckpoint_1"))
        with self.assertRaisesRegex(CheckpointError, "is not an object of level 'SP_A'"):
            parse_checkpoint(data)


class ReadTest(unittest.TestCase):
    def test_no_save_yet(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            self.assertIsNone(read_checkpoint(Path(root)))

    def test_reads_the_one_save(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root, "content/E030/4D5307D5/00000001/default_checkpoint.sav")
            directory.mkdir(parents=True)
            (directory / "Pla").write_bytes(save(checkpoint="WarCheckpoint_3"))
            self.assertEqual(read_checkpoint(Path(root)).name, "WarCheckpoint_3")

    def test_two_saves_refuse(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            for profile in ("E030", "E031"):
                directory = Path(root, f"content/{profile}/4D5307D5/00000001/default_checkpoint.sav")
                directory.mkdir(parents=True)
                (directory / "Pla").write_bytes(save())
            with self.assertRaisesRegex(CheckpointError, "2 checkpoint saves"):
                read_checkpoint(Path(root))


if __name__ == "__main__":
    unittest.main()

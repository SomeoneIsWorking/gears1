"""The RE image loader accepts the loaded layout and refuses the re-laid one."""

import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location("guest_image", TOOLS / "guest_image.py")
assert SPEC is not None and SPEC.loader is not None
guest_image = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(guest_image)

PE_OFFSET = 0x80
SIZE_OF_IMAGE = 0x2000


def make_pe(virtual_address: int, raw_offset: int, length: int) -> bytes:
    """One-section PE whose section claims VirtualAddress but sits at raw_offset."""
    data = bytearray(length)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, PE_OFFSET)
    data[PE_OFFSET : PE_OFFSET + 4] = b"PE\0\0"
    struct.pack_into("<H", data, PE_OFFSET + 6, 1)
    struct.pack_into("<H", data, PE_OFFSET + 20, 224)
    optional = PE_OFFSET + 24
    struct.pack_into("<I", data, optional + 56, SIZE_OF_IMAGE)
    section = optional + 224
    data[section : section + 8] = b".text\0\0\0"
    struct.pack_into("<IIII", data, section + 8, 0x200, virtual_address, 0x200, raw_offset)
    return bytes(data)


class GuestImageLayoutTest(unittest.TestCase):
    def load(self, data: bytes) -> bytes:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "image.bin"
            path.write_bytes(data)
            return guest_image.load_guest_image(path)

    def test_loaded_layout_is_accepted(self) -> None:
        loaded = make_pe(0x1000, 0x400, 0x600)
        self.assertEqual(self.load(loaded), loaded)

    def test_relaid_layout_is_refused_by_name(self) -> None:
        relaid = make_pe(0x1000, 0x400, SIZE_OF_IMAGE)
        with self.assertRaisesRegex(guest_image.GuestImageError, "VirtualAddress"):
            self.load(relaid)

    def test_image_without_displaced_sections_has_one_layout(self) -> None:
        identical = make_pe(0x1000, 0x1000, SIZE_OF_IMAGE)
        self.assertEqual(self.load(identical), identical)

    def test_missing_image_names_the_build_command(self) -> None:
        with self.assertRaisesRegex(guest_image.GuestImageError, "--image-out"):
            guest_image.load_guest_image(Path(tempfile.gettempdir()) / "absent-gears-image.bin")

    def test_non_pe_is_refused(self) -> None:
        with self.assertRaisesRegex(guest_image.GuestImageError, "DOS header"):
            self.load(b"\0" * 0x100)


if __name__ == "__main__":
    unittest.main()

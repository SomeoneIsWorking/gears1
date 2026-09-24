"""Decoding of single guest words, including the Xenon vector forms capstone lacks."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))

import capstone  # noqa: E402

import ppcdis  # noqa: E402


def decode(word: int) -> tuple[str, str]:
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    return ppcdis.decode(md, word.to_bytes(4, "big"), 0x82222350, word)


class DecodeTest(unittest.TestCase):
    def test_capstone_decodes_an_ordinary_word(self) -> None:
        self.assertEqual(decode(0x39440070), ("addi", "r10, r4, 0x70"))

    def test_names_the_unaligned_vector_loads(self) -> None:
        self.assertEqual(decode(0x7C005C0E), ("lvlx", "v0, r0, r11"))
        self.assertEqual(decode(0x7DAB3C4E), ("lvrx", "v13, r11, r7"))

    def test_joins_a_vmx128_register_split_across_the_word(self) -> None:
        # stvx128 v96: low bits 0 in 21-25, high bits 3 in 2-3.
        self.assertEqual(decode(0x100001C3 | (3 << 2) | (4 << 16) | (5 << 11)),
                         ("stvx128", "v96, r4, r5"))
        self.assertEqual(decode(0x100000C3 | (7 << 21)), ("lvx128", "v7, r0, r0"))

    def test_an_unknown_word_is_named_undecodable(self) -> None:
        self.assertEqual(decode(0x00000000), ("<undecodable>", ""))


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Raw PowerPC (big-endian, 32-bit VLE-less) disassembler over the flat guest image.

Ghidra's Disasm.py depends on Ghidra's own listing/flow reconstruction, which
silently degrades to a byte dump when the region was never successfully
disassembled (and DecompXbox.py's blr-stubbing of the save/restore helpers can
make the decompiler's view of such a function pure fiction). This tool reads the
image bytes directly and decodes them with capstone, so it is independent of any
Ghidra state.

Addresses are guest virtual addresses in the image as the XEX loader leaves it,
so this reads that loaded image through tools/guest_image.py, which refuses a
copy re-laid by section VirtualAddress rather than decoding the wrong bytes.

Usage:
    tools/ppcdis.py 0x8223B8A0 0x8223B940
    tools/ppcdis.py 0x8223B8A0 +0x80
    tools/ppcdis.py --image scratch/raw/gears_image.bin --base 0x82000000 0x8223B8A0 +0x40
"""
import argparse
import sys

import capstone

from guest_image import DEFAULT_BASE, DEFAULT_IMAGE, GuestImageError, load_guest_image, read_range


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("start")
    ap.add_argument("end", help="end VA (exclusive) or +LEN")
    ap.add_argument("--image", default=str(DEFAULT_IMAGE))
    ap.add_argument("--base", default=hex(DEFAULT_BASE))
    args = ap.parse_args()

    base = int(args.base, 0)
    start = int(args.start, 0)
    end = start + int(args.end[1:], 0) if args.end.startswith("+") else int(args.end, 0)

    try:
        data = read_range(load_guest_image(args.image), base, start, end)
    except GuestImageError as error:
        print(f"ppcdis: refusing: {error}", file=sys.stderr)
        return 2

    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    md.detail = False
    for offset in range(0, len(data) - 3, 4):
        address = start + offset
        word = int.from_bytes(data[offset:offset + 4], "big")
        mnemonic, operands = decode(md, data[offset:offset + 4], address, word)
        print("%08X  %08X  %-10s %s" % (address, word, mnemonic, operands))
    return 0


# Xenon's VMX128 loads and stores (Xenia's kVX128_1 format), which capstone
# does not decode: the opcode bits under VX128_1_MASK, a 7-bit vector register
# split across bits 21-25 (low) and 2-3 (high), then RA and RB.
VX128_1_MASK = 0xFC0007F3
VX128_1 = {
    0x10000003: "lvsl128", 0x10000043: "lvsr128", 0x10000083: "lvewx128",
    0x100000C3: "lvx128", 0x100002C3: "lvxl128", 0x10000403: "lvlx128",
    0x10000443: "lvrx128", 0x10000603: "lvlxl128", 0x10000643: "lvrxl128",
    0x10000183: "stvewx128", 0x100001C3: "stvx128", 0x100003C3: "stvxl128",
    0x10000503: "stvlx128", 0x10000543: "stvrx128", 0x10000703: "stvlxl128",
    0x10000743: "stvrxl128",
}


# The Cell/Xenon unaligned vector loads and stores (primary opcode 31, X form),
# by extended opcode; capstone does not decode these either.
VECTOR_EDGE_MASK = 0xFC0007FE
VECTOR_EDGE = {
    (31 << 26) | (xo << 1): name for xo, name in (
        (519, "lvlx"), (551, "lvrx"), (647, "stvlx"), (679, "stvrx"),
        (775, "lvlxl"), (807, "lvrxl"), (903, "stvlxl"), (935, "stvrxl"))
}


def decode(md: capstone.Cs, word_bytes: bytes, address: int, word: int) -> tuple[str, str]:
    """One instruction's mnemonic and operands; a word neither decoder knows is named as such."""

    for instruction in md.disasm(word_bytes, address):
        return instruction.mnemonic, instruction.op_str
    name = VX128_1.get(word & VX128_1_MASK)
    if name is not None:
        vector = ((word >> 21) & 31) | (((word >> 2) & 3) << 5)
        return name, f"v{vector}, r{(word >> 16) & 31}, r{(word >> 11) & 31}"
    name = VECTOR_EDGE.get(word & VECTOR_EDGE_MASK)
    if name is not None:
        return name, f"v{(word >> 21) & 31}, r{(word >> 16) & 31}, r{(word >> 11) & 31}"
    return "<undecodable>", ""


if __name__ == "__main__":
    sys.exit(main())

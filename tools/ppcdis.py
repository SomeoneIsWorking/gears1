#!/usr/bin/env python3
"""Raw PowerPC (big-endian, 32-bit VLE-less) disassembler over the flat guest image.

Ghidra's Disasm.py depends on Ghidra's own listing/flow reconstruction, which
silently degrades to a byte dump when the region was never successfully
disassembled (and DecompXbox.py's blr-stubbing of the save/restore helpers can
make the decompiler's view of such a function pure fiction). This tool reads the
image bytes directly and decodes them with capstone, so it is independent of any
Ghidra state.

Usage:
    tools/ppcdis.py 0x8223B8A0 0x8223B940
    tools/ppcdis.py 0x8223B8A0 +0x80
    tools/ppcdis.py --image scratch/raw/gears_image.bin --base 0x82000000 0x8223B8A0 +0x40
"""
import argparse
import sys

import capstone

DEFAULT_IMAGE = "scratch/raw/gears_image.bin"
DEFAULT_BASE = 0x82000000


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("start")
    ap.add_argument("end", help="end VA (exclusive) or +LEN")
    ap.add_argument("--image", default=DEFAULT_IMAGE)
    ap.add_argument("--base", default=hex(DEFAULT_BASE))
    args = ap.parse_args()

    base = int(args.base, 0)
    start = int(args.start, 0)
    end = start + int(args.end[1:], 0) if args.end.startswith("+") else int(args.end, 0)

    with open(args.image, "rb") as f:
        f.seek(start - base)
        data = f.read(end - start)

    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    md.detail = False
    seen = start
    for ins in md.disasm(data, start):
        word = int.from_bytes(data[ins.address - start:ins.address - start + 4], "big")
        print("%08X  %08X  %-10s %s" % (ins.address, word, ins.mnemonic, ins.op_str))
        seen = ins.address + ins.size
    if seen < end:
        print("%08X  <undecodable>" % seen)
    return 0


if __name__ == "__main__":
    sys.exit(main())

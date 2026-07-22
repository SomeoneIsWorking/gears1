#!/usr/bin/env python3
"""Extract Xbox 360 D3D9 shader containers (magic 0x102A11xx) from files.

Container layout, derived from the two built-ins embedded in the XEX and
cross-checked against several hundred containers in the cooked UE3 packages
(see docs/d3d-seam.md section 3):

    +0x00  u32  magic 0x102A11tt   tt = 0 pixel shader, 1 vertex shader
    +0x04  u32  headerSize         offset of the data blob from container start
    +0x08  u32  offset (unmapped)
    +0x0C  u32  0
    +0x10  u32  offset of the D3D9 constant table, minus 4
                 (that word holds the CTAB byte size; CTAB itself begins at +4,
                  and its Version word is the familiar 0xFFFF0300 / 0xFFFE0300)
    +0x14  u32  offset of a 0x28-byte section, or 0 if absent
    +0x18  u32  offset of the shader-info section (last in header, ~0x24-0x2C)
                  info[0] u32 constant-block byte size
                  info[1] u32 microcode byte size (always a multiple of 12)
                  info[2..9]  register-shaped words, not decoded here

    headerSize                     start of the float constant block
    headerSize + info[0]           start of the Xenos microcode
    headerSize + info[0] + info[1] end of the container

The microcode is the payload: Xenos ucode, NOT D3D9 bytecode. The
0xFFFF0300 / 0xFFFE0300 token in the CTAB is metadata only.

Usage:
    tools/shader_extract.py --out scratch/shaders/containers \\
        scratch/game/WarGame/CookedXenon/EngineMaterials.xxx
    tools/shader_extract.py --out DIR --image scratch/raw/gears_image.bin \\
        --at 0x39878 --at 0x39A40
"""
import argparse
import hashlib
import os
import struct
import sys

MAGIC_PREFIX = bytes.fromhex("102A11")
TYPE_NAMES = {0: "ps", 1: "vs"}


class BadContainer(Exception):
    pass


def parse(data, off):
    """Parse the container at `off`; return a dict, or raise BadContainer."""
    if off + 0x20 > len(data):
        raise BadContainer("truncated header")
    magic, hdr_size, _w8, w_c, ctab_off, _w14, info_off = struct.unpack_from(
        ">7I", data, off)
    if (magic & 0xFFFFFF00) != 0x102A1100:
        raise BadContainer("magic %08X" % magic)
    stype = magic & 0xFF
    if stype not in TYPE_NAMES:
        raise BadContainer("unknown shader type %d" % stype)
    if w_c != 0:
        raise BadContainer("+0x0C is %08X, expected 0" % w_c)
    if not (0x20 <= info_off < hdr_size) or info_off + 8 > hdr_size:
        raise BadContainer("info section %X outside header %X"
                           % (info_off, hdr_size))
    const_size, ucode_size = struct.unpack_from(">2I", data, off + info_off)
    if ucode_size == 0 or ucode_size % 12 != 0:
        raise BadContainer("microcode size %X is not a non-zero multiple of 12"
                           % ucode_size)
    if const_size % 4 != 0:
        raise BadContainer("constant block size %X is not a multiple of 4"
                           % const_size)
    total = hdr_size + const_size + ucode_size
    if off + total > len(data):
        raise BadContainer("blob runs past end of file")

    ctab_size = struct.unpack_from(">I", data, off + ctab_off)[0]
    version = struct.unpack_from(">I", data, off + ctab_off + 4 + 8)[0]
    expect = 0xFFFF0300 if stype == 0 else 0xFFFE0300
    if version != expect:
        raise BadContainer("CTAB version %08X, expected %08X for %s"
                           % (version, expect, TYPE_NAMES[stype]))

    ucode_off = hdr_size + const_size
    return {
        "offset": off,
        "type": TYPE_NAMES[stype],
        "total_size": total,
        "header_size": hdr_size,
        "ctab_off": ctab_off,
        "ctab_size": ctab_size,
        "const_size": const_size,
        "ucode_off": ucode_off,
        "ucode_size": ucode_size,
        "bytes": data[off:off + total],
        "ucode": data[off + ucode_off:off + ucode_off + ucode_size],
    }


def scan(data):
    off = 0
    while True:
        i = data.find(MAGIC_PREFIX, off)
        if i < 0:
            return
        off = i + 1
        try:
            yield parse(data, i)
        except BadContainer as e:
            yield {"offset": i, "error": str(e)}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*")
    ap.add_argument("--image", help="flat guest image to read --at offsets from")
    ap.add_argument("--at", action="append", default=[],
                    help="explicit container offset inside --image")
    ap.add_argument("--out", required=True, help="output directory")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    written, dup, bad = 0, 0, 0
    seen = set()
    counts = {"ps": 0, "vs": 0}

    def emit(c, origin):
        nonlocal written, dup
        digest = hashlib.sha1(c["ucode"]).hexdigest()[:16]
        name = "%s_%s.bin" % (c["type"], digest)
        path = os.path.join(args.out, name)
        if digest in seen:
            dup += 1
            return
        seen.add(digest)
        with open(path, "wb") as f:
            f.write(c["bytes"])
        counts[c["type"]] += 1
        written += 1
        print("%-40s +%08X %s ucode=%5d bytes -> %s"
              % (origin, c["offset"], c["type"], c["ucode_size"], name))

    sources = []
    for p in args.files:
        sources.append((p, open(p, "rb").read(), None))
    if args.at:
        if not args.image:
            ap.error("--at requires --image")
        img = open(args.image, "rb").read()
        sources.append((args.image, img, [int(a, 0) for a in args.at]))

    for origin, data, explicit in sources:
        if explicit is None:
            for c in scan(data):
                if "error" in c:
                    bad += 1
                    continue
                emit(c, origin)
        else:
            for o in explicit:
                emit(parse(data, o), origin)

    print("\n%d distinct microcode payloads written (%d pixel, %d vertex); "
          "%d duplicates skipped, %d magic hits rejected by the layout checks"
          % (written, counts["ps"], counts["vs"], dup, bad))
    return 0


if __name__ == "__main__":
    sys.exit(main())

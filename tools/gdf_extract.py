#!/usr/bin/env python3
"""Xbox 360 GDF (XGD) ISO reader.

Lists or extracts individual files from an Xbox 360 game disc image without
unpacking the whole 7.8 GB filesystem.

Usage:
    gdf_extract.py <iso> --list
    gdf_extract.py <iso> --extract default.xex --out <path>
    gdf_extract.py <iso> --extract-all <directory>

The ISO path defaults to $GEARS_ISO (see .env).  Nothing from the disc is ever
written inside the repo's tracked tree -- point --out at scratch/.
"""
import argparse
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Candidate offsets of the game partition inside the image file.
# 0            : raw GDF dump
# 0xFD90000    : XGD1 / XGD2 video-partition offset
# 0x02080000   : XGD2 alternate
# 0x18300000   : XGD3
CANDIDATE_BASES = (0x0, 0xFD90000, 0x02080000, 0x18300000)

ATTR_DIRECTORY = 0x10


def find_base(f):
    for base in CANDIDATE_BASES:
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return base
    raise SystemExit("no MICROSOFT*XBOX*MEDIA volume descriptor found; "
                     "not a recognised XGD image")


def read_volume(f, base):
    f.seek(base + 32 * SECTOR)
    hdr = f.read(SECTOR)
    if hdr[:20] != MAGIC:
        raise SystemExit("volume descriptor magic mismatch")
    root_sector, root_size = struct.unpack_from("<II", hdr, 20)
    return root_sector, root_size


def walk_dir(f, base, sector, size, prefix=""):
    """Yield (path, start_sector, size, attributes) for a directory table."""
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    out = []

    def visit(off):
        # Iterative stack to avoid deep recursion on large tables.
        stack = [off]
        while stack:
            o = stack.pop()
            if o * 4 + 14 > len(table):
                continue
            p = o * 4
            left, right, start, fsize, attr, namelen = struct.unpack_from(
                "<HHIIBB", table, p)
            if left == 0xFFFF:  # free/terminator entry
                continue
            name = table[p + 14:p + 14 + namelen].decode("latin-1")
            path = prefix + name
            out.append((path, start, fsize, attr))
            if left:
                stack.append(left)
            if right:
                stack.append(right)

    visit(0)
    result = []
    for path, start, fsize, attr in out:
        result.append((path, start, fsize, attr))
        if attr & ATTR_DIRECTORY and fsize:
            result.extend(walk_dir(f, base, start, fsize, path + "/"))
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iso", nargs="?", default=os.environ.get("GEARS_ISO"))
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--extract", help="path inside the disc, e.g. default.xex")
    ap.add_argument("--out", help="destination file for --extract")
    ap.add_argument("--extract-all", metavar="DIR",
                    help="extract the whole disc into DIR, preserving layout")
    args = ap.parse_args()

    if not args.iso:
        raise SystemExit("no ISO given and $GEARS_ISO is unset")

    with open(args.iso, "rb") as f:
        base = find_base(f)
        root_sector, root_size = read_volume(f, base)
        entries = walk_dir(f, base, root_sector, root_size)
        sys.stderr.write(
            f"partition base 0x{base:X}, root sector {root_sector}, "
            f"{len(entries)} entries\n")

        if args.list:
            for path, start, size, attr in sorted(entries):
                kind = "DIR " if attr & ATTR_DIRECTORY else "FILE"
                print(f"{kind} {size:>12} {path}")
            return

        if args.extract_all:
            root = args.extract_all
            files = [e for e in entries if not (e[3] & ATTR_DIRECTORY)]
            total = sum(e[2] for e in files)
            written = 0
            for i, (path, start, size, _attr) in enumerate(sorted(files), 1):
                dest = os.path.join(root, path)
                os.makedirs(os.path.dirname(dest) or root, exist_ok=True)
                # Skip files already present at the right size so an interrupted
                # extraction can be resumed rather than restarted.
                if os.path.exists(dest) and os.path.getsize(dest) == size:
                    written += size
                    continue
                f.seek(base + start * SECTOR)
                with open(dest, "wb") as out:
                    remaining = size
                    while remaining > 0:
                        chunk = f.read(min(remaining, 8 << 20))
                        if not chunk:
                            break
                        out.write(chunk)
                        remaining -= len(chunk)
                written += size
                if i % 100 == 0 or i == len(files):
                    sys.stderr.write(
                        f"\r{i}/{len(files)} files, "
                        f"{written/1e9:.2f}/{total/1e9:.2f} GB")
                    sys.stderr.flush()
            sys.stderr.write("\ndone\n")
            return

        if args.extract:
            want = args.extract.lower().replace("\\", "/")
            for path, start, size, attr in entries:
                if path.lower() == want and not (attr & ATTR_DIRECTORY):
                    if not args.out:
                        raise SystemExit("--out is required with --extract")
                    os.makedirs(os.path.dirname(os.path.abspath(args.out)),
                                exist_ok=True)
                    f.seek(base + start * SECTOR)
                    remaining = size
                    with open(args.out, "wb") as o:
                        while remaining:
                            chunk = f.read(min(1 << 20, remaining))
                            if not chunk:
                                raise SystemExit("short read -- truncated image")
                            o.write(chunk)
                            remaining -= len(chunk)
                    sys.stderr.write(f"wrote {size} bytes to {args.out}\n")
                    return
            raise SystemExit(f"{args.extract!r} not found on disc")

        raise SystemExit("nothing to do: pass --list or --extract")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Look for a draw's float constants as BYTES in a capture's guest memory.

Why this can settle something. The ALU constant file is filled two ways:
SET_CONSTANT carries the values inline in the ring buffer, LOAD_ALU_CONSTANT
loads them from guest physical memory. A capture stores guest memory but not
the ring, so finding a constant block IN the capture says the guest CPU
assembled those words in RAM -- which puts the value's origin on the CPU side
without paying for a 200-second live run.

WHAT A NEGATIVE HERE DOES NOT MEAN. Not finding it is not evidence the CPU did
not write it: the block may have arrived inline via SET_CONSTANT, or its page
may have been zero-elided out of the capture. The run prints how many bytes it
actually searched so the negative carries its denominator, and it refuses to
run at all rather than report "not found" for a file it could not read.

    tools/find_const_block.py <capture.gfr> [<capture.gfr> ...]
"""
import struct
import sys
from pathlib import Path

# The two pixel constants that black out play_v2.gfr (catalog #73), and the
# values every working capture has in the same slots. Searching for BOTH is the
# point: a search that has only ever been run against the case it expects to
# find has not been shown capable of reporting the other answer.
NAN_C8 = bytes.fromhex("ffc00000" * 3 + "00000000")
NAN_C7 = bytes.fromhex("00000000" * 3 + "3f000000")       # (0, 0, 0, 0.5)
GOOD_C7 = bytes.fromhex("3f800000" * 3 + "3f000000")      # (1, 1, 1, 0.5)
GOOD_C8 = bytes.fromhex("00000000" * 4)                   # (0, 0, 0, 0)

PATTERNS = [
    ("c7||c8 as play_v2 has them", NAN_C7 + NAN_C8),
    ("c8 alone, the three NaNs", NAN_C8),
    ("c7||c8 as every WORKING capture has them", GOOD_C7 + GOOD_C8),
    ("c7 alone, working value (1,1,1,0.5)", GOOD_C7),
]


MAGIC = b"GEARSFR1"


class GuestMap:
    """File offset -> guest physical address, from the capture's block table.

    Without this the offsets a search prints are offsets into a file, and the
    one thing anyone wants to do with a hit -- watch that address on a live run
    to catch the code that wrote it -- needs an ADDRESS. Parsing can fail (an
    older capture, a truncated file); when it does this says so and the search
    still runs with offsets only, rather than inventing addresses.
    """

    def __init__(self, blob: bytes):
        self.ok = False
        self.why = ""
        self.spans = []  # (file_start, file_end, guest_start)
        if blob[:8] != MAGIC:
            self.why = "not a GEARSFR1 capture"
            return
        try:
            (version, width, height, mirror) = struct.unpack_from("<4I", blob, 8)
            pos = 8 + 16
            if version >= 2:
                pos += 4  # frontBufferAddress
            elif version != 1:
                self.why = f"version {version} is not one this script knows"
                return
            (window,) = struct.unpack_from("<I", blob, pos); pos += 4
            pos += 8      # sequence (int64)
            pos += 4      # draw count
            (block_size,) = struct.unpack_from("<I", blob, pos); pos += 4
            (block_count,) = struct.unpack_from("<I", blob, pos); pos += 4
            if block_size == 0:
                self.why = "block size 0"
                return
            ids = struct.unpack_from(f"<{block_count}I", blob, pos)
            pos += 4 * block_count
            for b in ids:
                guest = b * block_size
                n = min(block_size, window - guest)
                self.spans.append((pos, pos + n, guest))
                pos += n
            self.ok = True
            self.summary = (f"version {version}, {width}x{height}, {block_count} blocks"
                            f" of {block_size:#x} covering {window:#x} bytes of guest window")
        except struct.error as e:
            self.why = f"header did not parse ({e})"

    def guest_of(self, off: int):
        for start, end, guest in self.spans:
            if start <= off < end:
                return guest + (off - start)
        return None


def find_all(hay: bytes, needle: bytes, limit: int = 8):
    """Every offset, capped -- but the CAP IS REPORTED, and the count is exact."""
    hits, i = [], hay.find(needle)
    while i >= 0:
        hits.append(i)
        i = hay.find(needle, i + 1)
    return hits[:limit], len(hits)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    rc = 0
    for arg in argv[1:]:
        p = Path(arg)
        if not p.is_file():
            # A search of a corpus that is not there searched NOTHING, and must
            # not be readable as "no matches".
            print(f"REFUSING to report on {p}: it does not exist. Nothing was searched.")
            rc = 1
            continue
        blob = p.read_bytes()
        gm = GuestMap(blob)
        print(f"== {p.name}: {len(blob):,} bytes searched "
              f"(the whole capture file, guest-memory blocks included) ==")
        if gm.ok:
            print(f"   block table: {gm.summary}")
        else:
            # A hit outside the mapped blocks is a hit in the file's OTHER
            # sections (microcode, register snapshots) and is not a guest
            # address at all. Saying which is the difference between a lead and
            # a wrong lead.
            print(f"   block table UNREADABLE ({gm.why}) -- offsets only, no addresses")
        for name, pat in PATTERNS:
            hits, total = find_all(blob, pat)
            if total:
                where = []
                for h in hits:
                    g = gm.guest_of(h) if gm.ok else None
                    where.append(f"{h:#x}" if g is None
                                 else f"{h:#x}=guest {g:#x}")
                more = f" (+{total - len(hits)} more)" if total > len(hits) else ""
                print(f"   FOUND  {total:5d}x  {name}\n"
                      f"            at {', '.join(where)}{more}")
            else:
                print(f"   absent          {name}  "
                      f"({len(pat)} bytes, big-endian, not present anywhere)")
        print()
    print("An offset with no `=guest` fell OUTSIDE the block table: it is in the\n"
          "capture's other sections (register snapshots, microcode), not in guest\n"
          "memory, and must not be read as an address.")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))

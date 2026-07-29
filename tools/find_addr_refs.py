#!/usr/bin/env python3
"""Find the code that materialises a given guest address.

PowerPC has no 32-bit immediate, so an address is built from a pair:

    lis  rD, high          (addis rD, r0, high)
    addi rD, rD, low       (or ori rD, rD, low)

A byte search for the address therefore finds nothing -- it never appears as
four contiguous bytes. Ghidra's reference database resolves these pairs, but
only after a full auto-analysis pass, which costs twenty minutes on an image
this size and is wasted when the question is "who refers to this one address".

This scans the image directly for the pair. It reports every site whose
computed address matches, so a string constant or a device register can be
traced back to the code that uses it.

    tools/find_addr_refs.py <image> <base> <address> [address ...]
    tools/find_addr_refs.py scratch/raw/gears_image.bin 0x82000000 0x820bda98

Both shapes that build an address are recognised:

    lis  rA, hi ; addi rA, rA, lo      -- the address computed into a register
    lis  rA, hi ; stw  rD, lo(rA)      -- the address used directly by a load
                                          or store

The second was missing until it cost a session: a global whose only WRITER used
`lis`+`stw` was reported as having no writers at all, and reasoning was built on
that absence. An instrument that under-reports silently is worse than no
instrument, because "no hits" reads as an answer.
"""
import struct
import sys

# The pair can be separated by unrelated instructions, but not many: compilers
# emit the halves close together, and a wide window costs false positives where
# the register was reused for something else in between.
WINDOW = 12


def sign16(value):
    return value - 0x10000 if value & 0x8000 else value


def scan(data, base, targets):
    count = len(data) // 4
    words = struct.unpack(">%dI" % count, data[:count * 4])

    # index -> (register, high half) for every lis seen recently
    pending = {}
    hits = {t: [] for t in targets}

    for i, insn in enumerate(words):
        opcode = insn >> 26
        d = (insn >> 21) & 0x1F
        a = (insn >> 16) & 0x1F
        imm = insn & 0xFFFF

        if opcode == 15 and a == 0:          # lis rD, imm
            pending[d] = (i, imm)
            continue

        # A LOAD OR STORE COMPLETES THE ADDRESS TOO, and missing this made the
        # tool under-report SILENTLY -- the worst way for an instrument to be
        # wrong. `lis rA,hi ; stw rD,lo(rA)` writes to hi:lo just as
        # `lis rA,hi ; addi rA,rA,lo` computes it, and only the second was
        # recognised. That is how "only ONE reference to 0x82BFAD3C in the whole
        # image" was concluded, when the site that actually SETS it
        # (0x82207DBC, `stw r11,-21188(r10)`) was one of these.
        #
        # Opcodes 32..55 are the D-form integer and float load/stores: lwz(32)
        # lwzu lbz lbzu stw(36) stwu stb stbu lhz lhzu lha lhau sth sthu lmw stmw
        # lfs lfsu lfd lfdu stfs stfsu stfd stfdu. All share the rD,imm(rA) shape.
        if 32 <= opcode <= 55:
            entry = pending.get(a)
            if entry is not None and i - entry[0] <= WINDOW:
                address = ((entry[1] << 16) + sign16(imm)) & 0xFFFFFFFF
                if address in hits:
                    hits[address].append(base + i * 4)
            # The base register survives a load/store, so `pending` is NOT
            # cleared here -- one `lis` commonly serves several accesses to
            # neighbouring fields of the same structure.
            continue

        if opcode in (14, 24):               # addi rD,rA,imm / ori rA,rS,imm
            # addi reads rA and writes rD; ori reads rS (the D field) and
            # writes rA. Normalising them lets one branch handle both.
            source, low = (a, sign16(imm)) if opcode == 14 else (d, imm)
            entry = pending.get(source)
            if entry is not None and i - entry[0] <= WINDOW:
                address = ((entry[1] << 16) + low) & 0xFFFFFFFF
                if address in hits:
                    hits[address].append(base + entry[0] * 4)

        # A write to the register invalidates the half-built address.
        written = a if opcode == 24 else d
        if written in pending and opcode not in (15,):
            if opcode in (14, 24) and written == (a if opcode == 24 else d):
                pending.pop(written, None)

    return hits


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2

    image, base = sys.argv[1], int(sys.argv[2], 0)
    targets = [int(x, 0) for x in sys.argv[3:]]

    with open(image, "rb") as handle:
        data = handle.read()

    hits = scan(data, base, targets)
    for target in targets:
        sites = hits[target]
        if not sites:
            print("%#x: no references found" % target)
        for site in sites:
            print("%#x: referenced at %#x" % (target, site))
    return 0


if __name__ == "__main__":
    sys.exit(main())

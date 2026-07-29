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
            # The base register survives a plain load/store, so `pending` is NOT
            # cleared for it -- one `lis` commonly serves several accesses to
            # neighbouring fields of the same structure. What DOES clear it is
            # the destination of a load and the base of an update form, both of
            # which genuinely overwrite the half-built address. Recorded AFTER
            # the hit above, because the old value is what formed the address.
            for reg in D_FORM_WRITES_GPR(opcode, d, a):
                pending.pop(reg, None)
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
            pending.pop(d if opcode == 14 else a, None)
            continue

        # A write to the register invalidates the half-built address. Only
        # instructions whose destination register is CERTAIN are handled: a
        # wrong invalidation here would drop a real reference, and this tool has
        # already cost a session by under-reporting once. Anything not listed
        # leaves `pending` alone, which can only cost a false positive -- and a
        # false positive is visible to the reader, where a false negative is not.
        if opcode == 31 and ((insn >> 1) & 0x3FF) in X_FORM_WRITES_RD:
            pending.pop(d, None)
        elif opcode in (7, 8, 12, 13, 28, 29):   # mulli subfic addic addic. andi. andis.
            pending.pop(d if opcode in (7, 8, 12, 13) else a, None)

    return hits


# D-form opcodes whose GPR destination is written. The float loads and stores
# (48..55) write an FPR, so their `d` field must NOT invalidate a GPR -- doing so
# would silently drop references built on a register that was never touched.
_LOADS_WRITING_RD = {32, 33, 34, 35, 40, 41, 42, 43}
_UPDATE_FORMS = {33, 35, 37, 39, 41, 43, 45, 49, 51, 53, 55}


def D_FORM_WRITES_GPR(opcode, d, a):
    out = []
    if opcode in _LOADS_WRITING_RD:
        out.append(d)
    if opcode == 46:                  # lmw rD,off(rA): loads rD..r31
        out.extend(range(d, 32))
    if opcode in _UPDATE_FORMS:
        out.append(a)
    return out


# X-form (opcode 31) extended opcodes that write rD. Stores, compares and
# trap/cache ops are deliberately absent: their `d` field is a SOURCE.
X_FORM_WRITES_RD = {
    23, 55, 87, 119, 279, 311, 343, 375,      # lwzx lwzux lbzx lbzux lhzx lhzux lhax lhaux
    266, 10, 40, 8, 104, 235, 75, 11, 459,    # add addc subf subfc neg mullw mulhw mulhwu divwu
    491, 138, 234,                            # divw adde subfe
    28, 60, 124, 284, 316, 412, 444, 476,     # and andc nor eqv xor orc or nand
    24, 536, 792, 824,                        # slw srw sraw srawi
    954, 922, 26, 58, 986,                    # extsb extsh cntlzw cntlzd extsw
    339, 371, 595, 659,                       # mfspr mftb mfsr mfsrin
}


# WHAT A "no references found" DOES NOT COVER. Printed next to the negative
# result, not only in the docstring, because that is the moment a reader decides
# whether the absence means anything.
BLIND_SPOTS = """  this scan CANNOT see, so "none" is not "nowhere":
    - a lis/addi pair more than %d instructions apart (WINDOW)
    - indexed forms: `lwzx/stwx rD,rA,rB`, where the offset is in a register
    - an address loaded from memory (a pointer in .data, a TOC/GOT entry, a
      relocation, a vtable or jump-table slot) -- no immediate pair exists
    - an address reached as base+offset from a DIFFERENT nearby global
    - anything outside the bytes handed to this tool: check the image really
      covers the address, and that `base` is the right load address""" % WINDOW


def selftest():
    """Feed the scanner cases it MUST report, and one it must not.

    A scanner that silently matches nothing looks exactly like an image with no
    references. This makes the difference observable: run it and a broken build
    of this file fails loudly instead of printing "no references found" forever.
    That is the whole reason it exists -- this tool has already produced a false
    negative that cost a session.
    """
    def word(*fields):
        return struct.pack(">I", fields[0])

    def lis(rd, hi):
        return struct.pack(">I", (15 << 26) | (rd << 21) | (0 << 16) | (hi & 0xFFFF))

    def dform(op, rd, ra, off):
        return struct.pack(">I", (op << 26) | (rd << 21) | (ra << 16) | (off & 0xFFFF))

    TARGET = 0x82BFAD3C          # hi 0x82C0, lo -21188 == 0xAD3C - 0x10000
    hi, lo = 0x82C0, -21188
    BASE = 0x82000000
    cases = [
        # (name, code words, expected number of hits)
        ("lis+addi -- the address computed into a register",
         lis(10, hi) + dform(14, 3, 10, lo), 1),
        ("lis+stw -- the shape that was invisible until it cost a session",
         lis(10, hi) + dform(36, 11, 10, lo), 1),
        ("lis+lwz -- a reader through the same displacement",
         lis(10, hi) + dform(32, 3, 10, lo), 1),
        ("lis+ori",
         lis(10, hi) + struct.pack(">I", (24 << 26) | (10 << 21) | (10 << 16)
                                   | (0xAD3C)), 0),   # ori builds 0x82C0AD3C, not the target
        ("one lis serving two accesses to the same global",
         lis(10, hi) + dform(32, 3, 10, lo) + dform(36, 4, 10, lo), 2),
        ("base clobbered by a load before the displacement -- must NOT match",
         lis(10, hi) + dform(32, 10, 10, 0) + dform(36, 11, 10, lo), 0),
        ("halves too far apart -- a known blind spot, must NOT match",
         lis(10, hi) + b"\x60\x00\x00\x00" * (WINDOW + 2) + dform(36, 11, 10, lo), 0),
        ("an unrelated global -- must NOT match",
         lis(10, 0x8100) + dform(36, 11, 10, 0x10), 0),
    ]
    failures = 0
    for name, code, expected in cases:
        got = len(scan(code, BASE, [TARGET])[TARGET])
        ok = got == expected
        failures += not ok
        print("  %-4s %s (expected %d hit(s), got %d)"
              % ("ok" if ok else "FAIL", name, expected, got))
    if failures:
        print("\n%d self-test case(s) FAILED: this scanner cannot be trusted to"
              " report a negative." % failures)
        return 1
    print("\nself-test passed: the scanner demonstrably reports both address"
          " shapes, and rejects a clobbered base and an out-of-window pair.\n"
          "It still cannot see:\n" + BLIND_SPOTS)
    return 0


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--selftest":
        return selftest()
    if len(sys.argv) < 4:
        print(__doc__)
        return 2

    image, base = sys.argv[1], int(sys.argv[2], 0)
    targets = [int(x, 0) for x in sys.argv[3:]]

    with open(image, "rb") as handle:
        data = handle.read()

    if len(data) % 4:
        print("warning: image is %d bytes, not a multiple of 4; the last %d "
              "bytes were not scanned" % (len(data), len(data) % 4))

    hits = scan(data, base, targets)
    negatives = 0
    for target in targets:
        sites = hits[target]
        if not sites:
            negatives += 1
            if not (base <= target < base + (len(data) // 4) * 4):
                print("%#x: no references found -- AND IT IS OUTSIDE THE SCANNED"
                      " RANGE %#x..%#x, so this result says nothing at all"
                      % (target, base, base + (len(data) // 4) * 4))
            else:
                print("%#x: no references found" % target)
        for site in sites:
            print("%#x: referenced at %#x" % (target, site))
    if negatives:
        # The limits belong HERE, beside the negative, not only in --help.
        print("\n%d address(es) reported no references." % negatives)
        print(BLIND_SPOTS)
        print("  Run `%s --selftest` to confirm this scanner still finds the"
              " shapes it claims to." % sys.argv[0])
    return 0


if __name__ == "__main__":
    sys.exit(main())

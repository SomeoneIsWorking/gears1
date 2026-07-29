#!/usr/bin/env python3
"""Scan a big-endian PPC image for device-window accesses.

Finds every `lis rX, IMM` whose IMM is in the device window range
(0x7FC0..0x7FFF), then reports subsequent loads/stores (stw/stwbrx/lwz/lwbrx,
including indexed forms) that use rX as the base register before rX is
overwritten. Reports the effective register offset where determinable.

Usage: scan_mmio_stores.py <image.bin> <base_va> [--lis-lo HEX --lis-hi HEX]
"""
import sys
import struct

def main():
    image_path = sys.argv[1]
    base_va = int(sys.argv[2], 16)
    lis_lo, lis_hi = 0x7FC0, 0x7FFF
    data = open(image_path, 'rb').read()
    n = len(data) // 4
    words = struct.unpack('>%dI' % n, data[:n*4])
    # Counted so a silent run is not read as "this image touches no device
    # registers". Zero bases found and zero accesses found are different
    # failures -- the first says the window range or the image is wrong, the
    # second says the bases exist but nothing was matched near them -- and
    # printing nothing at all conflated both with a genuine clean image.
    bases_found = 0
    accesses = 0

    for i, w in enumerate(words):
        # addis rD, r0, imm  (lis)
        if (w >> 26) == 15 and ((w >> 16) & 31) == 0:
            imm = w & 0xFFFF
            if not (lis_lo <= imm <= lis_hi):
                continue
            rx = (w >> 21) & 31
            bases_found += 1
            # scan forward until rx is clobbered or 64 insns
            for j in range(i + 1, min(i + 65, n)):
                v = words[j]
                op = v >> 26
                ra = (v >> 16) & 31
                rd = (v >> 21) & 31
                va = base_va + j * 4
                # D-form loads/stores using rx as base
                if ra == rx and op in (32, 36, 40, 44, 34, 38, 33, 37):
                    name = {32:'lwz',33:'lwzu',36:'stw',37:'stwu',
                            40:'lhz',44:'sth',34:'lbz',38:'stb'}[op]
                    d = v & 0xFFFF
                    if d >= 0x8000: d -= 0x10000
                    ea = (imm << 16) + d
                    accesses += 1
                    print('%08X  %-6s r%d, 0x%X(r%d)   ea=%08X  (lis@%08X)'
                          % (va, name, rd, v & 0xFFFF, ra, ea, base_va + i*4))
                # addi rY, rx, imm -- address materialization
                if op == 14 and ra == rx:
                    d = v & 0xFFFF
                    if d >= 0x8000: d -= 0x10000
                    accesses += 1
                    print('%08X  addi   r%d, r%d, 0x%X  ea=%08X  (lis@%08X)'
                          % (va, rd, ra, v & 0xFFFF, (imm << 16) + d,
                             base_va + i*4))
                # X-form (op 31): stwbrx=662 lwbrx=534 stwx=151 lwzx=23
                if op == 31:
                    xo = (v >> 1) & 0x3FF
                    rb = (v >> 11) & 31
                    if xo in (662, 534, 151, 23) and (ra == rx or rb == rx):
                        name = {662:'stwbrx',534:'lwbrx',151:'stwx',23:'lwzx'}[xo]
                        accesses += 1
                        print('%08X  %-6s r%d, r%d, r%d   base=%04X0000  (lis@%08X)'
                              % (va, name, rd, ra, rb, imm, base_va + i*4))
                # rx clobbered? (rd == rx for arith/load ops that write rd)
                if op in (14, 15, 12, 13, 32, 33, 34, 35, 40, 41, 42, 43, 46) and rd == rx:
                    break
                if op == 31 and ((v >> 21) & 31) == rx:
                    xo = (v >> 1) & 0x3FF
                    # X-form ops that write rd/ra... conservative: break on any
                    # load or arithmetic writing rx
                    if xo in (23, 534, 87, 279, 343, 266, 40, 444, 316, 28):
                        break
                # branch with link or unconditional branch ends the window
                if op == 18 and (v & 1):  # bl
                    continue
                if op == 18 and not (v & 1):  # b
                    break
                if op == 19 and ((v >> 1) & 0x3FF) in (16, 528) and not (v & 1):
                    break  # blr/bctr

    print()
    print('scanned %d instruction word(s) from %08X: %d lis into the device '
          'window %04X..%04X, %d access(es) reported'
          % (n, base_va, bases_found, lis_lo, lis_hi, accesses))
    if bases_found == 0:
        print('NO DEVICE-WINDOW BASE REGISTER WAS BUILT ANYWHERE. That is a '
              'failed scan far more often than it is a clean image: check the '
              'file, the base VA, and that the window really is %04X..%04X.'
              % (lis_lo, lis_hi))
    elif accesses == 0:
        print('Bases exist but nothing matched near them.')
    if accesses == 0:
        print('This scan cannot see, so "none" is not "nowhere":')
        print('  - a base register held across a `bl` is followed anyway, and a '
              'base built more than 64 instructions before its use is not')
        print('  - only lwz/lwzu/lbz/lbzu/lhz/stw/stwu/sth/stb and '
              'stwbrx/lwbrx/stwx/lwzx are recognised; other forms are invisible')
        print('  - a device address loaded from memory, or computed from a '
              'register the caller passed in, has no lis to key on at all')

if __name__ == '__main__':
    main()

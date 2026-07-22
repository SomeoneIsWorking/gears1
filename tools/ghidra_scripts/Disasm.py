#@runtime Jython
# Print raw instructions in [GEARS_DISASM_START, GEARS_DISASM_END).
#
# Needed because DecompXbox.py overwrites the save/restore helper ranges with
# `blr`, which makes the decompiler's view of any function whose body Ghidra
# failed to rebuild pure fiction. When a decompilation looks nonsensical, read
# the actual instructions here before drawing a conclusion from it.
import os

af = currentProgram.getAddressFactory().getDefaultAddressSpace()
listing = currentProgram.getListing()

start = int(os.environ["GEARS_DISASM_START"], 16)
end = int(os.environ["GEARS_DISASM_END"], 16)

lo = af.getAddress("0x%X" % start)
hi = af.getAddress("0x%X" % (end - 1))

# Rebuild flow over the window: whatever is recorded may predate the helper
# stubbing, in which case the listing stops at the first prologue call.
listing.clearCodeUnits(lo, hi, False)
disassemble(lo)

va = start
while va < end:
    a = af.getAddress("0x%X" % va)
    cu = listing.getCodeUnitAt(a)
    if cu is None:
        print("%08X  <none>" % va)
        va += 4
        continue
    print("%08X  %s" % (va, cu.toString()))
    va += cu.getLength()

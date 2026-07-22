#@runtime Jython
# Xbox 360 code calls __savegprlr_NN / __restgprlr_NN (and the fpr/vmx variants)
# in prologue and epilogue. Those helpers restore LR and effectively return to
# the caller's caller, which Ghidra's decompiler cannot follow -- so it treats
# them as non-returning and discards the entire calling function's body.
#
# Clearing the noreturn flag does not help; the inference is inside the
# decompiler, not the function database. What does work is making the helpers
# trivially returning: overwrite every instruction in their ranges with `blr`
# (0x4E800020) inside the Ghidra program only.
#
# This loses the save/restore semantics in the decompilation, which is fine --
# they are prologue/epilogue bookkeeping, not logic, and dropping them makes the
# output more readable rather than less. The on-disk image is untouched.
from ghidra.program.model.symbol import SourceType

# jarray with SIGNED byte values: a Jython str literal does not convert to a
# Java byte[] correctly here and silently writes zeros, which disassemble to an
# invalid instruction and stop flow analysis rather than returning.
from jarray import array
BLR = array([0x4e, -0x80, 0x00, 0x20], "b")  # blr = 0x4E800020

# From config/gears.toml, byte-pattern scanned by tools/xex_probe.
RANGES = [
    (0x828D27E0, 0x828D2850),  # __savegprlr_14 .. __restgprlr_* tail
    (0x828D4200, 0x828D4290),  # __savefpr_14   .. __restfpr_* tail
    (0x828D7860, 0x828D7BE0),  # __savevmx_14   .. __restvmx_64 tail
]

af = currentProgram.getAddressFactory().getDefaultAddressSpace()
mem = currentProgram.getMemory()
listing = currentProgram.getListing()

patched = 0
for lo, hi in RANGES:
    # Clear existing instructions so the new bytes are re-disassembled cleanly.
    start = af.getAddress("0x%X" % lo)
    end = af.getAddress("0x%X" % (hi - 1))
    listing.clearCodeUnits(start, end, False)

    va = lo
    while va < hi:
        a = af.getAddress("0x%X" % va)
        mem.setBytes(a, BLR)
        va += 4
        patched += 1

for lo, hi in RANGES:
    disassemble(af.getAddress("0x%X" % lo))

print("STUBBED %d instructions across %d ranges" % (patched, len(RANGES)))

#@runtime Jython
# Xbox 360 binaries call __savegprlr_NN / __restgprlr_NN style helpers in the
# prologue and epilogue. Ghidra's analysis marks them noreturn, which makes every
# caller decompile to a single call and discards the body. Clearing the flag over
# the helper ranges restores the real decompilation.
from ghidra.program.model.symbol import SourceType

af = currentProgram.getAddressFactory().getDefaultAddressSpace()
fm = currentProgram.getFunctionManager()

# From config/gears.toml, byte-pattern scanned by tools/xex_probe.
RANGES = [
    (0x828D27E0, 0x828D2850),  # save/restgprlr
    (0x828D4200, 0x828D4290),  # save/restfpr
    (0x828D7860, 0x828D7BE0),  # save/restvmx
]

fixed = 0
for lo, hi in RANGES:
    a = af.getAddress("0x%X" % lo)
    end = af.getAddress("0x%X" % hi)
    fn = fm.getFunctionContaining(a)
    it = fm.getFunctions(a, True)
    while it.hasNext():
        f = it.next()
        if f.getEntryPoint().getOffset() >= hi:
            break
        if f.hasNoReturn():
            f.setNoReturn(False)
            fixed += 1
print("CLEARED_NORETURN %d" % fixed)

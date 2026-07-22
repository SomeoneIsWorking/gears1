#@runtime Jython
# One-shot Xbox 360 decompilation: stub the save/restore helpers, rebuild the
# target functions' boundaries, and decompile -- all in a single script run, so
# nothing depends on headless persisting memory edits between invocations.
#
# Why the stubbing is needed: Xbox 360 code calls __savegprlr_NN / __restgprlr_NN
# in prologue and epilogue. Those restore LR and effectively return to the
# caller's caller, which the decompiler cannot follow, so it treats them as
# non-returning and discards the whole calling function's body. Overwriting them
# with `blr` makes them trivially returning. The on-disk image is untouched.
import os
from jarray import array
from ghidra.app.cmd.function import CreateFunctionCmd
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

af = currentProgram.getAddressFactory().getDefaultAddressSpace()
mem = currentProgram.getMemory()
listing = currentProgram.getListing()
fm = currentProgram.getFunctionManager()

def addr(v):
    return af.getAddress("0x%X" % v)

# From config/gears.toml, byte-pattern scanned by tools/xex_probe.
RANGES = [
    (0x828D27E0, 0x828D2850),  # __savegprlr_14 .. __restgprlr_* tail
    (0x828D4200, 0x828D4290),  # __savefpr_14   .. __restfpr_* tail
    (0x828D7860, 0x828D7BE0),  # __savevmx_14   .. __restvmx_64 tail
]

# GhidraScript.setByte is used rather than Memory.setBytes: the latter silently
# wrote nothing here (a probe immediately afterwards, in the same script, still
# read zeros), while setByte goes through the script's own transaction handling.
BLR_BYTES = [0x4E, -0x80, 0x00, 0x20]  # blr = 0x4E800020, as signed bytes

for lo, hi in RANGES:
    listing.clearCodeUnits(addr(lo), addr(hi - 1), False)
    va = lo
    while va < hi:
        for i in range(4):
            setByte(addr(va + i), BLR_BYTES[i])
        va += 4

# Verify in-process rather than in a later run, so a write failure cannot be
# mistaken for a save failure.
# A Jython bytearray silently reads back as zeros here; getBytes needs a real
# Java byte[]. This bug made every earlier verification report a failed write.
from jarray import zeros
probe = zeros(4, "b")
mem.getBytes(addr(RANGES[0][0] + 0x38), probe)
print("PROBE %02x%02x%02x%02x (want 4e800020)" %
      (probe[0] & 0xFF, probe[1] & 0xFF, probe[2] & 0xFF, probe[3] & 0xFF))

for lo, hi in RANGES:
    disassemble(addr(lo))

di = DecompInterface()
di.openProgram(currentProgram)
mon = ConsoleTaskMonitor()

outdir = os.environ.get("GEARS_DECOMP_OUT", "build/decomp")
if not os.path.isdir(outdir):
    os.makedirs(outdir)

for t in [x.strip() for x in os.environ.get("GEARS_DECOMP_TARGETS", "").split(",") if x.strip()]:
    a = af.getAddress(t)
    f = fm.getFunctionContaining(a)
    if f is not None:
        fm.removeFunction(f.getEntryPoint())

    # The instructions here were disassembled while the prologue call looked
    # non-returning, so the recorded flow stops at it. Clearing and
    # re-disassembling rebuilds the flow with the stubbed helpers in place;
    # without this the recreated function is only the prologue.
    span = int(os.environ.get("GEARS_SPAN", "0x3000"), 16)
    listing.clearCodeUnits(a, af.getAddress("0x%X" % (a.getOffset() + span)), False)
    disassemble(a)
    CreateFunctionCmd(a).applyTo(currentProgram, monitor)
    f = fm.getFunctionContaining(a)
    if f is None:
        print("NOFUNC %s" % t)
        continue

    res = di.decompileFunction(f, 180, mon)
    if not res.decompileCompleted():
        print("FAILED %s: %s" % (t, res.getErrorMessage()))
        continue

    c = res.getDecompiledFunction().getC()
    path = os.path.join(outdir, "%s.c" % t)
    fh = open(path, "w")
    fh.write(c)
    fh.close()
    print("OK %s bytes=%d lines=%d -> %s" %
          (t, f.getBody().getNumAddresses(), c.count("\n"), path))

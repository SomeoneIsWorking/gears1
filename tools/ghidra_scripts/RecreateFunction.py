#@runtime Jython
# After stubbing the save/restore helpers, existing function boundaries are still
# the ones Ghidra derived while it believed the prologue call never returned --
# so bodies stop at the prologue. Deleting and recreating the function forces it
# to re-derive the extent with the corrected control flow.
import os
from ghidra.app.cmd.function import CreateFunctionCmd

af = currentProgram.getAddressFactory().getDefaultAddressSpace()
fm = currentProgram.getFunctionManager()

for t in [x.strip() for x in os.environ.get("GEARS_RECREATE","").split(",") if x.strip()]:
    a = af.getAddress(t)
    f = fm.getFunctionContaining(a)
    if f is not None:
        entry = f.getEntryPoint()
        fm.removeFunction(entry)
    else:
        entry = a
    cmd = CreateFunctionCmd(entry)
    ok = cmd.applyTo(currentProgram, monitor)
    nf = fm.getFunctionContaining(entry)
    size = nf.getBody().getNumAddresses() if nf is not None else 0
    print("RECREATED %s ok=%s bytes=%d" % (t, ok, size))

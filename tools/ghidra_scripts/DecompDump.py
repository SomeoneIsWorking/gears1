#@runtime Jython
# Decompile the function containing each VA in GEARS_DECOMP_TARGETS (comma-separated
# hex) to build/decomp/<VA>.c. Env-driven because Ghidra's headless interface exposes
# no script argv.
import os
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

targets = os.environ.get("GEARS_DECOMP_TARGETS", "")
outdir = os.environ.get("GEARS_DECOMP_OUT", "build/decomp")
if not os.path.isdir(outdir):
    os.makedirs(outdir)

af = currentProgram.getAddressFactory().getDefaultAddressSpace()
fm = currentProgram.getFunctionManager()
di = DecompInterface()
di.openProgram(currentProgram)
mon = ConsoleTaskMonitor()

for t in [x.strip() for x in targets.split(",") if x.strip()]:
    va = af.getAddress(t)
    fn = fm.getFunctionContaining(va)
    if fn is None:
        print("NOFUNC %s" % t)
        continue
    res = di.decompileFunction(fn, 120, mon)
    if not res.decompileCompleted():
        print("FAILED %s: %s" % (t, res.getErrorMessage()))
        continue
    path = os.path.join(outdir, "%s.c" % t)
    f = open(path, "w")
    f.write(res.getDecompiledFunction().getC())
    f.close()
    print("OK %s -> %s (%s)" % (t, path, fn.getName()))

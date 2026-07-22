#@runtime Jython
import os
af = currentProgram.getAddressFactory().getDefaultAddressSpace()
fm = currentProgram.getFunctionManager()
for t in [x.strip() for x in os.environ.get("GEARS_NORETURN_VAS","").split(",") if x.strip()]:
    a = af.getAddress(t)
    f = fm.getFunctionContaining(a)
    if f is None:
        print("NOFUNC %s" % t); continue
    print("%s -> %s noreturn=%s" % (t, f.getName(), f.hasNoReturn()))
    if f.hasNoReturn():
        f.setNoReturn(False)
        print("  cleared")

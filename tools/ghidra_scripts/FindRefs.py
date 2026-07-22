#@runtime Jython
# List every reference to each address in GEARS_REF_TARGETS, with the function
# that contains it.
#
# Ghidra's reference database already resolves the lis/addi pairs PowerPC uses
# to materialise an address, so this finds constant references that a byte-level
# search for the literal would miss.
import os

af = currentProgram.getAddressFactory().getDefaultAddressSpace()
fm = currentProgram.getFunctionManager()
rm = currentProgram.getReferenceManager()

for target in [x.strip() for x in os.environ.get("GEARS_REF_TARGETS", "").split(",") if x.strip()]:
    a = af.getAddress(target)
    refs = rm.getReferencesTo(a)
    found = False
    for ref in refs:
        found = True
        source = ref.getFromAddress()
        fn = fm.getFunctionContaining(source)
        print("%s <- %s  %s  in %s" % (
            target, source, ref.getReferenceType(),
            fn.getEntryPoint() if fn is not None else "(no function)"))
    if not found:
        print("%s <- no references" % target)

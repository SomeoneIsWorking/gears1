#@runtime Jython
import os
af = currentProgram.getAddressFactory().getDefaultAddressSpace()
mem = currentProgram.getMemory()
for t in [x.strip() for x in os.environ.get("GEARS_PEEK","").split(",") if x.strip()]:
    a = af.getAddress(t)
    b = bytearray(4)
    mem.getBytes(a, b)
    print("%s: %02x%02x%02x%02x" % (t, b[0]&0xff, b[1]&0xff, b[2]&0xff, b[3]&0xff))

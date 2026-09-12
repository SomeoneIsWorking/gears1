#@runtime Jython
import os

from jarray import zeros

program = globals().get("currentProgram")
if program is None:
    raise RuntimeError("PeekBytes requires an open Ghidra program")
af = program.getAddressFactory().getDefaultAddressSpace()
mem = program.getMemory()
targets = [x.strip() for x in os.environ.get("GEARS_PEEK", "").split(",") if x.strip()]
if not targets:
    raise ValueError("GEARS_PEEK must name at least one guest address")

for t in targets:
    a = af.getAddress(t)
    if a is None or not mem.contains(a):
        raise ValueError("unmapped guest address: " + t)
    # Jython bytearray does not receive Memory.getBytes writes; use Java byte[].
    b = zeros(4, "b")
    if mem.getBytes(a, b) != 4:
        raise ValueError("could not read four guest bytes at " + t)
    print(t + ": " + "".join(format(byte & 0xFF, "02x") for byte in b))
print("scanned " + str(len(targets)) + " requested guest addresses")

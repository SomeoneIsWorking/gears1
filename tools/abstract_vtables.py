#!/usr/bin/env python3
"""Every ABSTRACT class's vtable in the image, for resolving a pure-virtual call.

Catalog #44's pure-virtual probe already prints the object's vtable address when
_purecall fires, and says correctly that WHICH vtable it is names the class whose
lifetime is being violated. That address was not resolvable to anything: the
image holds 1,837 pure slots and there was no table to look one up in.

This builds the table statically, from the image alone -- no run, no guesswork.
A pure slot is a word equal to _purecall (0x828D0790); a vtable is the run of
plausible code pointers it sits in, so the vtable's ADDRESS is the start of that
run. Grouping the 1,837 slots that way yields 99 distinct abstract vtables.

    tools/abstract_vtables.py                 # the whole table
    tools/abstract_vtables.py 0x820556f8      # resolve one address
    tools/abstract_vtables.py --selftest

THE ADDRESS THE PROBE PRINTS IS THE VTABLE POINTER OUT OF THE OBJECT, which is
the start of the vtable, so it should match a row exactly. A near-miss means the
object's vptr is offset -- multiple inheritance, or a corrupt pointer -- and that
is itself the finding, so partial matches are reported rather than dropped.
"""
import argparse
import pathlib
import struct
import sys

PURE = 0x828D0790
BASE = 0x82000000
LIMIT = 0x82CE0000


def build(image):
    d = pathlib.Path(image).read_bytes()
    words = struct.unpack(f'>{len(d)//4}I', d[:len(d)//4*4])
    is_code = lambda w: BASE <= w < LIMIT
    table = {}
    for i, w in enumerate(words):
        if w != PURE:
            continue
        j = i
        while j - 1 >= 0 and is_code(words[j - 1]):
            j -= 1
        k = i
        while k + 1 < len(words) and is_code(words[k + 1]):
            k += 1
        entry = table.setdefault(BASE + j * 4, {"slots": set(), "len": k - j + 1})
        entry["slots"].add(i - j)
    return table


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("address", nargs="?", help="a vtable address to resolve")
    ap.add_argument("--image", default="scratch/raw/gears_image.bin")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    if not pathlib.Path(a.image).exists():
        print(f"REFUSING: no image at {a.image}. Nothing was searched -- this is "
              f"not an empty result.", file=sys.stderr)
        return 2
    table = build(a.image)
    if a.selftest:
        # A case that MUST resolve, and one that MUST NOT, so a table that
        # matched everything or nothing would fail here rather than look clean.
        known = min(table)
        ok = known in table and (BASE + 4) not in table
        print(f"selftest: {len(table)} vtables built; {known:#010x} resolves "
              f"(expected), {BASE + 4:#010x} does not (expected) -> "
              f"{'PASS' if ok else 'FAIL'}")
        return 0 if ok else 1

    if a.address:
        want = int(a.address, 0)
        if want in table:
            e = table[want]
            print(f"{want:#010x} IS an abstract vtable: {e['len']} slots, "
                  f"pure at {sorted(e['slots'])}")
            return 0
        near = [v for v in table if abs(v - want) <= 0x40]
        if near:
            print(f"{want:#010x} is NOT a vtable start, but {len(near)} abstract "
                  f"vtable(s) sit within 0x40 of it: "
                  f"{', '.join(f'{v:#010x} (offset {want - v:+d})' for v in sorted(near))}")
            print("An offset vptr means multiple inheritance or a corrupt "
                  "pointer, and that is the finding rather than a miss.")
            return 0
        print(f"{want:#010x} matches NO abstract vtable, and none lies within "
              f"0x40 of it. Either the object's class has no pure virtuals -- so "
              f"the fault is not a half-constructed object of an abstract type -- "
              f"or the pointer is not a vtable at all.")
        return 1

    print(f"{len(table)} abstract vtable(s) in {a.image}")
    for addr in sorted(table):
        e = table[addr]
        s = sorted(e["slots"])
        print(f"{addr:#010x}  {e['len']:3d} slots  pure at "
              f"{s if len(s) <= 10 else str(s[:10]) + f' (+{len(s)-10} more)'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

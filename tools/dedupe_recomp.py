#!/usr/bin/env python3
"""Remove duplicate function definitions from the XenonRecomp output.

Cause (measured 2026-07-22): a recompiler run that emits FEWER translation
units than a previous run leaves the previous run's extra `ppc_recomp.N.cpp`
files behind, and `runtime/CMakeLists.txt` globs whatever is in the directory.
Here ppc_recomp.191/192 were leftovers whose 353 functions were all already
defined in 189/190 -- so the build compiled two vintages of the same code.

Each definition is a STRONG `__imp__sub_X`, so the link survives only while the
linker happens not to extract both archive members. It does extract both as
soon as anything changes extraction order -- e.g. a native override that
super-calls `__imp__sub_X`. The link is order-dependent, which is a latent
defect rather than a style issue.

This pass removes a definition only when an earlier translation unit already
defines the same function with a byte-identical body, and then deletes any
translation unit that is left contributing nothing. A definition whose body
DIFFERS is reported and never touched, because that is no longer a stale
duplicate but a genuine disagreement that a human must look at.

    tools/dedupe_recomp.py [--dir scratch/ppc] [--check]
"""
import argparse
import glob
import os
import re
import sys

DEF = re.compile(
    r'__attribute__\(\(alias\("__imp__(sub_[0-9A-Fa-f]+)"\)\)\) '
    r'PPC_WEAK_FUNC\(\1\);\n'
    r'PPC_FUNC_IMPL\(__imp__\1\) \{.*?\n\}\n',
    re.S)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dir", default="scratch/ppc")
    ap.add_argument("--check", action="store_true",
                    help="report duplicates and exit non-zero, changing nothing")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.dir, "ppc_recomp.*.cpp")),
                   key=lambda p: int(re.search(r'\.(\d+)\.cpp$', p).group(1)))

    seen: dict[str, tuple[str, str]] = {}   # name -> (file, body)
    removed = 0
    conflicts = []

    for path in files:
        text = open(path).read()
        out = []
        last = 0
        changed = False
        for m in DEF.finditer(text):
            name = m.group(1)
            body = m.group(0)
            if name not in seen:
                seen[name] = (path, body)
                continue
            first_path, first_body = seen[name]
            if first_body != body:
                conflicts.append((name, first_path, path))
                continue
            removed += 1
            if not args.check:
                out.append(text[last:m.start()])
                last = m.end()
                changed = True
        if changed:
            out.append(text[last:])
            open(path, "w").write("".join(out))

    # A translation unit that no longer defines anything the earlier units do
    # not already define contributes nothing but duplicate symbols. That is the
    # leftover-file case; drop the whole file.
    stale = []
    for path in files:
        if args.check and not os.path.exists(path):
            continue
        text = open(path).read()
        names = [m.group(1) for m in DEF.finditer(text)]
        if all(seen.get(n, (path,))[0] != path for n in names):
            stale.append(path)
            if not args.check:
                os.remove(path)
    for path in stale:
        print(f"{'stale' if args.check else 'removed stale'} translation unit "
              f"{path} (defines nothing not already defined earlier)")

    unresolved = [c for c in conflicts if c[2] not in stale]
    if unresolved:
        for name, a, b in unresolved:
            print(f"CONFLICT {name}: {a} and {b} differ -- not touched",
                  file=sys.stderr)
        return 2
    if args.check:
        if removed:
            print(f"{removed} duplicate function definitions present")
            return 1
        print("no duplicate function definitions")
        return 0
    print(f"removed {removed} duplicate function definitions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

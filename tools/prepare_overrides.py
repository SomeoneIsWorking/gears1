#!/usr/bin/env python3
"""Make the functions we override natively actually overridable.

XenonRecomp emits every function as

    __attribute__((alias("__imp__sub_X"))) PPC_WEAK_FUNC(sub_X);
    PPC_FUNC_IMPL(__imp__sub_X) { ...body... }

so that a native replacement can define a strong `sub_X` and win at link time.
That works only for call sites in OTHER translation units. Inside the defining
translation unit clang folds a call to an alias into a call to its aliasee, so
the call becomes `call __imp__sub_X` in the object file and no link-time
symbol can intercept it. Verified in the binary: `__imp__sub_82221980` calls
`__imp__sub_822218C0` directly, and a strong `sub_822218C0` was never entered
(measured: 9 probes registered, 0 calls, over the whole movie phase).

Since the D3D layer is one dense cluster of functions, most of its calls are
intra-TU, which is exactly the set an HLE seam has to intercept.

This pass deletes the alias line for the functions listed as overridden, so the
name `sub_X` has no definition in the generated sources at all and every call
site -- intra-TU included -- becomes an external reference that the native
definition satisfies. The body stays as `__imp__sub_X` for the super-call.

The general fix belongs in the recompiler: emit a weak forwarding *function*
(`PPC_WEAK_FUNC(sub_X) { __imp__sub_X(ctx, base); }`) instead of an alias, which
is not foldable. Until that lands this keeps the seam honest, and it fails loud:
an override whose alias was not stripped is reported by name.

    tools/prepare_overrides.py [--dir scratch/ppc] [--overrides runtime/hle_d3d.cpp]
"""
import argparse
import glob
import os
import re
import sys

MACRO = re.compile(r'^\s*GEARS_HLE_(?:TRACE|OVERRIDE)\((?P<addr>[0-9A-Fa-f]{8})\)',
                   re.M)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dir", default="scratch/ppc")
    ap.add_argument("--overrides", default="runtime/hle_d3d.cpp")
    args = ap.parse_args()

    wanted = {m.group("addr").upper() for m in
              MACRO.finditer(open(args.overrides).read())}
    if not wanted:
        print("no overrides declared; nothing to do")
        return 0

    stripped = set()
    for path in glob.glob(os.path.join(args.dir, "ppc_recomp.*.cpp")):
        text = open(path).read()
        out = text
        for addr in wanted:
            alias = (f'__attribute__((alias("__imp__sub_{addr}"))) '
                     f'PPC_WEAK_FUNC(sub_{addr});\n')
            if alias in out:
                out = out.replace(alias,
                                  f'// sub_{addr}: alias removed by '
                                  f'tools/prepare_overrides.py (native override)\n')
                stripped.add(addr)
        if out != text:
            open(path, "w").write(out)

    missing = sorted(a for a in wanted
                     if a not in stripped and not _already_stripped(args.dir, a))
    for addr in sorted(stripped):
        print(f"stripped alias for sub_{addr}")
    if missing:
        for addr in missing:
            print(f"ERROR: no definition of sub_{addr} found in {args.dir}",
                  file=sys.stderr)
        return 1
    return 0


def _already_stripped(directory: str, addr: str) -> bool:
    marker = f"// sub_{addr}: alias removed"
    for path in glob.glob(os.path.join(directory, "ppc_recomp.*.cpp")):
        with open(path) as f:
            if marker in f.read():
                return True
    return False


if __name__ == "__main__":
    raise SystemExit(main())

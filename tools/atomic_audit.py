#!/usr/bin/env python3
"""Classify every lwarx/ldarx -> stwcx./stdcx. reservation window in the
recompiled PPC sources, so we can decide whether the CAS-based translation of
store-conditional can diverge from a true reservation.

The classification is a DATAFLOW test, not an opcode whitelist: a window counts
as benign only if the stored register is actually derived from the loaded one.
Checking opcodes alone cannot see the shape that matters most here -- a compare
-and-swap whose new value was loaded through the old pointer, where the window
body is only `cmpw; bne` and every opcode looks harmless.

A window is BENIGN under a value-CAS if the stored value is a pure function of
the value the paired load observed AT THE SAME ADDRESS (add/sub/or/and/xor/
rlwinm/li...), because the retry loop then converges to the same result whether
or not an ABA happened.  It is ABA-SENSITIVE if the stored value comes from
anywhere else (another register loaded from another address, a pointer, a
counter loaded separately) -- there the guest is really asking "has anyone
touched this since I looked", which a value CAS cannot answer.
"""
import re, sys, glob, os, collections

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "scratch", "ppc")

fn_re   = re.compile(r'^PPC_FUNC_IMPL\(__imp__(\w+)\)')
insn_re = re.compile(r'^\s*// (\S+)\s*(.*)$')

# instructions whose result is a pure function of already-live registers
PURE = re.compile(r'^(addi?|addic\.?|subf\w*|neg|or\w*|and\w*|xor\w*|nor|nand|'
                  r'eqv|rlwinm\.?|rlwimi|rldicl?|rldicr|sld|slw|srw|sraw\w*|'
                  r'srd|srad\w*|extsb|extsh|extsw|mr|li|lis|cntlzw|cmp\w*|'
                  r'ori|oris|andi\.|andis\.|xori|xoris|nop|clrlwi|mulli|not)')

windows = []
for path in sorted(glob.glob(os.path.join(SRC, "ppc_recomp.*.cpp"))):
    fn = "?"
    cur = None
    for ln, line in enumerate(open(path, errors="replace"), 1):
        m = fn_re.match(line)
        if m:
            fn = m.group(1); cur = None; continue
        m = insn_re.match(line)
        if not m:
            continue
        op, rest = m.group(1), m.group(2)
        if op in ("lwarx", "ldarx"):
            cur = dict(file=os.path.basename(path), line=ln, fn=fn,
                       load=op + " " + rest, body=[], store=None)
            continue
        if cur is None:
            continue
        if op in ("stwcx.", "stdcx."):
            cur["store"] = op + " " + rest
            windows.append(cur); cur = None
            continue
        cur["body"].append(op + " " + rest)
        if len(cur["body"]) > 60:      # runaway: no paired store nearby
            cur["body"].append("...ABANDONED...")
            windows.append(cur); cur = None

benign, suspect, orphan = [], [], []
for w in windows:
    if w["store"] is None or "...ABANDONED..." in w["body"]:
        orphan.append(w); continue
    ld_rt = w["load"].split()[1].split(",")[0]
    st_rs = w["store"].split()[1].split(",")[0]
    ops = [b.split()[0] for b in w["body"]]
    impure = [o for o in ops if not PURE.match(o)]

    # THE DATAFLOW CHECK THE DOCSTRING PROMISES. An earlier version of this
    # file computed ld_rt and st_rs and then never used them: it classified on
    # "no impure opcodes" alone. That made it STRUCTURALLY BLIND to the case it
    # exists to find -- a value CAS whose new value is a pointer loaded THROUGH
    # the old one (the Treiber-stack pop), whose window body is just a compare
    # and a branch. A detector that cannot see its own target reports zero
    # forever, which reads as "nothing here" rather than "I cannot tell".
    derived = {ld_rt}
    for b in w["body"]:
        parts = b.replace(",", " ").split()
        if len(parts) < 2 or not PURE.match(parts[0]):
            continue
        dest, sources = parts[1], parts[2:]
        if any(src in derived for src in sources):
            derived.add(dest)
        elif dest in derived:
            # Overwritten from something unrelated: no longer the loaded value.
            derived.discard(dest)

    if not impure and st_rs in derived:
        benign.append(w)
    else:
        w["why"] = ("stored register is not derived from the loaded value"
                    if st_rs not in derived else
                    f"impure ops in the window: {sorted(set(impure))}")
        suspect.append(w)

print(f"windows found: {len(windows)}  benign(pure RMW): {len(benign)}  "
      f"suspect: {len(suspect)}  orphan/unpaired: {len(orphan)}")
print()
print("=== BENIGN body-op histogram ===")
h = collections.Counter(b.split()[0] for w in benign for b in w["body"])
for k, v in h.most_common():
    print(f"  {v:5d}  {k}")
print()
print("=== SUSPECT windows (value-CAS may diverge from a real reservation) ===")
for w in suspect:
    print(f"  {w['fn']} @ {w['file']}:{w['line']}")
    print(f"      {w['load']}")
    for b in w["body"]:
        print(f"      {b}")
    print(f"      {w['store']}")
print()
print("=== ORPHAN (larx with no stcx. within 60 insns) ===")
for w in orphan:
    print(f"  {w['fn']} @ {w['file']}:{w['line']}  {w['load']}")

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
_ARGS = [a for a in sys.argv[1:] if not a.startswith("-")]
SRC = _ARGS[0] if _ARGS else os.path.join(REPO, "scratch", "ppc")

fn_re   = re.compile(r'^PPC_FUNC_IMPL\(__imp__(\w+)\)')
insn_re = re.compile(r'^\s*// (\S+)\s*(.*)$')

# instructions whose result is a pure function of already-live registers
PURE = re.compile(r'^(addi?|addic\.?|subf\w*|neg|or\w*|and\w*|xor\w*|nor|nand|'
                  r'eqv|rlwinm\.?|rlwimi|rldicl?|rldicr|sld|slw|srw|sraw\w*|'
                  r'srd|srad\w*|extsb|extsh|extsw|mr|li|lis|cntlzw|cmp\w*|'
                  r'ori|oris|andi\.|andis\.|xori|xoris|nop|clrlwi|mulli|not)')


def scan_lines(filename, lines):
    """Parse recompiled C++ into reservation windows.

    Returns (windows, insn_lines_seen). The second value exists because this
    parser reads the ORIGINAL INSTRUCTION out of the `// <insn>` comment the
    recompiler emits above each translated line -- and nothing else in the file
    is legible to it. If that comment format ever changes, or the wrong source
    directory is given, every regex misses and the tool prints
    "windows found: 0", which reads as "this image has no atomics" rather than
    "I parsed nothing at all". The count makes those two distinguishable.
    """
    windows = []
    insn_lines = 0
    fn = "?"
    cur = None
    for ln, line in enumerate(lines, 1):
        m = fn_re.match(line)
        if m:
            fn = m.group(1); cur = None; continue
        m = insn_re.match(line)
        if not m:
            continue
        insn_lines += 1
        op, rest = m.group(1), m.group(2)
        if op in ("lwarx", "ldarx"):
            cur = dict(file=filename, line=ln, fn=fn,
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
    if cur is not None:                # ran off the end of the file
        cur["body"].append("...ABANDONED...")
        windows.append(cur)
    return windows, insn_lines


def classify(w):
    """-> 'benign' | 'suspect' | 'orphan', setting w['why'] when suspect."""
    if w["store"] is None or "...ABANDONED..." in w["body"]:
        return "orphan"
    ld_rt = w["load"].split()[1].split(",")[0]
    st_rs = w["store"].split()[1].split(",")[0]
    ops = [b.split()[0] for b in w["body"]]
    impure = [o for o in ops if not PURE.match(o)]

    # THE DATAFLOW CHECK THE DOCSTRING PROMISES. An earlier version of this file
    # computed ld_rt and st_rs and then never used them: it classified on "no
    # impure opcodes" alone. That made it STRUCTURALLY BLIND to the case it
    # exists to find -- a value CAS whose new value is a pointer loaded THROUGH
    # the old one (the Treiber-stack pop), whose window body is just a compare,
    # a branch and a load. A detector that cannot see its own target reports
    # zero forever, which reads as "nothing here" rather than "I cannot tell".
    # `--selftest` now feeds it exactly that shape, so the check is
    # demonstrably alive rather than merely written down.
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
        return "benign"
    w["why"] = ("stored register is not derived from the loaded value"
                if st_rs not in derived else
                f"impure ops in the window: {sorted(set(impure))}")
    return "suspect"


# What a "suspect: 0" does NOT cover. Printed WITH the result, because that is
# the moment a reader decides whether the zero means anything.
BLIND_SPOTS = """  a zero here is bounded by what this audit can see:
    - it reads the `// <insn>` comments only; a recompiler that stops emitting
      them, or a wrong source directory, yields zero windows and zero suspects
    - a window whose store is more than 60 instructions after its load is
      counted ORPHAN, not classified -- check that count is not hiding cases
    - dataflow is register-name matching inside one window; a value routed
      through memory, through a condition register, or through a call is
      invisible to it
    - lwarx/stwcx. reached only through a hand-written override or the
      interpreter is not in these sources at all
    - it says nothing about whether the RUNTIME's CAS is implemented correctly,
      only about what the guest code asks for"""


# Four windows, one per verdict, written in the exact shape the parser reads.
SELFTEST_SOURCE = """
PPC_FUNC_IMPL(__imp__sub_selftest_treiber_pop)
\t// lwarx r3,0,r4
\t// cmpwi r3,0
\t// lwz r5,0(r3)
\t// stwcx. r5,0,r4
PPC_FUNC_IMPL(__imp__sub_selftest_pure_increment)
\t// lwarx r3,0,r4
\t// addi r3,r3,1
\t// stwcx. r3,0,r4
PPC_FUNC_IMPL(__imp__sub_selftest_unrelated_counter)
\t// lwarx r3,0,r4
\t// mr r6,r7
\t// stwcx. r6,0,r4
PPC_FUNC_IMPL(__imp__sub_selftest_orphan)
\t// lwarx r3,0,r4
\t// blr
"""


def selftest():
    """Feed the classifier one case per verdict, including the one it exists for.

    A detector whose whole output is a count is indistinguishable, when the
    count is zero, from a detector that cannot count. The Treiber-stack pop
    below is exactly the shape the previous version of this file could never
    report: the stored register comes from THROUGH the loaded pointer rather
    than from it. If that case does not come back `suspect`, the dataflow check
    is dead again and every "suspect: 0" this tool prints is worthless.
    """
    expected = {
        "sub_selftest_treiber_pop": "suspect",
        "sub_selftest_pure_increment": "benign",
        "sub_selftest_unrelated_counter": "suspect",
        "sub_selftest_orphan": "orphan",
    }
    windows, insn_lines = scan_lines("<selftest>", SELFTEST_SOURCE.splitlines())
    print("self-test: parsed %d instruction comments into %d window(s)"
          % (insn_lines, len(windows)))
    failures = 0
    seen = set()
    for w in windows:
        got = classify(w)
        want = expected.get(w["fn"])
        seen.add(w["fn"])
        ok = got == want
        failures += not ok
        print("  %-4s %-34s expected %-8s got %-8s %s"
              % ("ok" if ok else "FAIL", w["fn"], want, got, w.get("why", "")))
    for name in expected:
        if name not in seen:
            failures += 1
            print("  FAIL %-34s produced NO WINDOW AT ALL -- the parser is not "
                  "reading these sources" % name)
    if failures:
        print("\n%d self-test case(s) FAILED: this audit cannot be trusted to "
              "report a negative." % failures)
        return 1
    print("\nself-test passed: the dataflow check demonstrably separates a pure "
          "RMW from a Treiber-stack pop, so a 'suspect: 0' on real sources is a "
          "measurement rather than a blind spot. It remains bounded by:")
    print(BLIND_SPOTS)
    return 0


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()

    files = sorted(glob.glob(os.path.join(SRC, "ppc_recomp.*.cpp")))
    if not files:
        # THE FAILED SCAN AND THE CLEAN IMAGE USED TO PRINT THE SAME THING.
        print("NO SOURCES MATCHED %s/ppc_recomp.*.cpp -- this is a failed scan, "
              "not an image with no atomics." % SRC)
        return 2

    windows = []
    insn_lines = 0
    for path in files:
        with open(path, errors="replace") as handle:
            w, n = scan_lines(os.path.basename(path), handle)
        windows += w
        insn_lines += n

    if insn_lines == 0:
        print("%d file(s) scanned but NOT ONE `// <insn>` comment was parsed -- "
              "the recompiler's comment format has changed and this audit is "
              "reading nothing. Every count below would be meaningless."
              % len(files))
        return 2

    benign, suspect, orphan = [], [], []
    bucket = {"benign": benign, "suspect": suspect, "orphan": orphan}
    for w in windows:
        bucket[classify(w)].append(w)

    print("%d file(s), %d instruction comments parsed" % (len(files), insn_lines))
    print("windows found: %d  benign(pure RMW): %d  suspect: %d  "
          "orphan/unpaired: %d"
          % (len(windows), len(benign), len(suspect), len(orphan)))
    print()
    print("=== BENIGN body-op histogram ===")
    h = collections.Counter(b.split()[0] for w in benign for b in w["body"])
    for k, v in h.most_common():
        print("  %5d  %s" % (v, k))
    print()
    print("=== SUSPECT windows (value-CAS may diverge from a real reservation) ===")
    for w in suspect:
        print("  %s @ %s:%d  -- %s" % (w['fn'], w['file'], w['line'], w['why']))
        print("      %s" % w['load'])
        for b in w["body"]:
            print("      %s" % b)
        print("      %s" % w['store'])
    if not suspect:
        print("  (none)")
        print(BLIND_SPOTS)
        print("  Run `tools/atomic_audit.py --selftest` to confirm the dataflow "
              "check still separates the shapes it claims to.")
    print()
    print("=== ORPHAN (larx with no stcx. within 60 insns) ===")
    for w in orphan:
        print("  %s @ %s:%d  %s" % (w['fn'], w['file'], w['line'], w['load']))
    if not orphan:
        print("  (none)")
    return 0


if __name__ == "__main__":
    sys.exit(main())


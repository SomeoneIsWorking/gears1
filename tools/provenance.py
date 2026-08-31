#!/usr/bin/env python3
"""Which RUN did these artefacts come from? -- so a cross-run pair cannot be
joined silently.

WHY THIS EXISTS (catalog #62, claim C042). Two oracle runs reach different game
moments: both emulators advance the guest by wall-clock delta, so frame N of one
run is not the moment frame N of another is (#84, #98). On 2026-08-12 a capture
taken at 11:34 was compared against resolve dumps from an 11:54 oracle run --
and that later run had OVERWRITTEN the very camera file the capture was gated
to. The comparison scored a log-luminance correlation of 0.07 where a matching
pair scores 0.93, and two wrong conclusions were published before the file
timestamps gave it away. Nothing in either side's filenames recorded the run.

    tools/provenance.py stamp <dir> --role ours|theirs --pair <id> [--note k=v]
    tools/provenance.py check <dir-a> <dir-b>
    tools/provenance.py show  <dir>
    tools/provenance.py --selftest

THE PAIR ID IS THE JOIN. Two directories belong together iff they carry the same
`pair`. `layer_capture.py` drives both sides in one invocation and stamps one id
on both; the split path -- an oracle run producing a camera file, then a separate
camera-gated capture of ours -- must carry the oracle run's id forward, which is
what `--pair` is for.

AN UNSTAMPED DIRECTORY IS NOT A PASS. `check` distinguishes three outcomes and
exits differently for each: MATCH (0), MISMATCH (2), and UNKNOWN (3, one or both
unstamped). Reporting "no problem found" for a directory that carries no
provenance at all is exactly the silent join this file exists to prevent.
"""
import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import time

STAMP = "PROVENANCE.json"


def git_head(repo):
    try:
        return subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"],
                              capture_output=True, text=True,
                              timeout=10).stdout.strip() or None
    except Exception:
        return None


def file_digest(path):
    p = pathlib.Path(path)
    if not p.is_file():
        return None
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:16]


def do_stamp(a):
    d = pathlib.Path(a.dir)
    if not d.is_dir():
        raise SystemExit(f"REFUSING: {a.dir} is not a directory. NOTHING was "
                         f"stamped.")
    notes = {}
    for kv in a.note or []:
        if "=" not in kv:
            raise SystemExit(f"REFUSING: --note {kv!r} is not key=value.")
        k, v = kv.split("=", 1)
        notes[k] = v
    # A camera file is COPIED IN, not referenced: the incident this tool exists
    # for happened because the referenced file was overwritten by a later run.
    if a.camera:
        src = pathlib.Path(a.camera)
        if not src.is_file():
            raise SystemExit(f"REFUSING: --camera {a.camera} does not exist, so "
                             f"the capture's own camera cannot be frozen "
                             f"alongside it. NOTHING was stamped.")
        notes["camera_sha256_16"] = file_digest(src)
        notes["camera_name"] = src.name
        (d / "camera.txt").write_bytes(src.read_bytes())
    rec = {
        "pair": a.pair,
        "role": a.role,
        "stamped_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "git_head": git_head(pathlib.Path(__file__).resolve().parent.parent),
        "host_pid": os.getpid(),
        "notes": notes,
    }
    (d / STAMP).write_text(json.dumps(rec, indent=2) + "\n")
    print(f"stamped {d}/{STAMP}: pair={a.pair} role={a.role}"
          + (f" camera={notes.get('camera_name')} "
             f"sha={notes.get('camera_sha256_16')}" if a.camera else ""))
    return 0


def read_stamp(d):
    p = pathlib.Path(d) / STAMP
    if not p.is_file():
        return None
    try:
        return json.loads(p.read_text())
    except Exception as e:
        raise SystemExit(f"REFUSING: {p} exists but is unreadable ({e}). That is "
                         f"a corrupt stamp, not a missing one.")


def do_check(a):
    sa, sb = read_stamp(a.a), read_stamp(a.b)
    missing = [d for d, s in ((a.a, sa), (a.b, sb)) if s is None]
    if missing:
        print(f"PROVENANCE UNKNOWN: no {STAMP} in " + ", ".join(missing),
              file=sys.stderr)
        print(f"  This is NOT a pass. These artefacts carry no record of which "
              f"run produced them, so they may be from different oracle runs -- "
              f"which reach different game moments (#84/#98) and score ~0.07 "
              f"correlation where a real pair scores ~0.93. Stamp them at "
              f"capture time with `provenance.py stamp`, or check the file "
              f"mtimes by hand before quoting any cross-side number.",
              file=sys.stderr)
        return 3
    print(f"A {a.a}: pair={sa['pair']} role={sa['role']} at {sa['stamped_utc']}")
    print(f"B {a.b}: pair={sb['pair']} role={sb['role']} at {sb['stamped_utc']}")
    if sa["pair"] != sb["pair"]:
        print(f"PROVENANCE MISMATCH: {sa['pair']!r} != {sb['pair']!r}. These are "
              f"from DIFFERENT RUNS and must not be compared pixelwise.",
              file=sys.stderr)
        return 2
    if sa["role"] == sb["role"]:
        print(f"PROVENANCE MISMATCH: both carry role={sa['role']!r}; a pair is "
              f"one 'ours' and one 'theirs'.", file=sys.stderr)
        return 2
    ca = sa["notes"].get("camera_sha256_16")
    cb = sb["notes"].get("camera_sha256_16")
    if ca and cb and ca != cb:
        print(f"PROVENANCE MISMATCH: camera digests differ ({ca} vs {cb}).",
              file=sys.stderr)
        return 2
    print("PROVENANCE MATCH: same pair id, complementary roles"
          + (f", same camera ({ca})" if ca and cb else ""))
    return 0


def do_show(a):
    s = read_stamp(a.dir)
    if s is None:
        print(f"{a.dir}: UNSTAMPED -- no {STAMP}. Provenance unknown.",
              file=sys.stderr)
        return 3
    print(json.dumps(s, indent=2))
    return 0


def selftest():
    """Drive all three outcomes, because a checker that only ever prints MATCH
    is the failure this file is about."""
    import tempfile
    rc = 0
    # Not /tmp: it is a small tmpfs here and run artefacts belong in scratch/.
    root = pathlib.Path(__file__).resolve().parent.parent / "scratch"
    base = str(root) if root.is_dir() else None
    with tempfile.TemporaryDirectory(dir=base) as td:
        t = pathlib.Path(td)
        for n in ("ours", "theirs", "other", "bare"):
            (t / n).mkdir()
        cam = t / "cam.txt"
        cam.write_text("c[230]=(1, 0, 0, 0)\n")
        ns = argparse.Namespace
        do_stamp(ns(dir=str(t / "ours"), role="ours", pair="RUN1",
                    note=None, camera=str(cam)))
        do_stamp(ns(dir=str(t / "theirs"), role="theirs", pair="RUN1",
                    note=None, camera=str(cam)))
        do_stamp(ns(dir=str(t / "other"), role="theirs", pair="RUN2",
                    note=None, camera=None))
        cases = [("MATCH", "ours", "theirs", 0),
                 ("MISMATCH (different run)", "ours", "other", 2),
                 ("UNKNOWN (unstamped)", "ours", "bare", 3)]
        for label, x, y, want in cases:
            got = do_check(ns(a=str(t / x), b=str(t / y)))
            ok = got == want
            rc |= 0 if ok else 1
            print(f"  {label}: exit {got}, expected {want} -> "
                  f"{'PASS' if ok else 'FAIL'}\n")
        frozen = (t / "ours" / "camera.txt").read_text()
        ok = frozen == cam.read_text()
        rc |= 0 if ok else 1
        print(f"  camera frozen into the capture dir (survives the source being "
              f"overwritten) -> {'PASS' if ok else 'FAIL'}")
        cam.write_text("OVERWRITTEN BY A LATER RUN\n")
        still = (t / "ours" / "camera.txt").read_text()
        ok2 = still == frozen
        rc |= 0 if ok2 else 1
        print(f"  ... and still intact after the source IS overwritten -> "
              f"{'PASS' if ok2 else 'FAIL'}")
    print(f"selftest: {'PASS' if rc == 0 else 'FAIL'} (all three outcomes "
          f"driven, not reasoned about)")
    return rc


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd")
    s = sub.add_parser("stamp")
    s.add_argument("dir")
    s.add_argument("--role", required=True, choices=("ours", "theirs"))
    s.add_argument("--pair", required=True,
                   help="the run id shared by both sides of one comparison")
    s.add_argument("--note", action="append", metavar="KEY=VALUE")
    s.add_argument("--camera", help="camera file to FREEZE into the directory")
    c = sub.add_parser("check")
    c.add_argument("a")
    c.add_argument("b")
    w = sub.add_parser("show")
    w.add_argument("dir")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.cmd == "stamp":
        return do_stamp(a)
    if a.cmd == "check":
        return do_check(a)
    if a.cmd == "show":
        return do_show(a)
    ap.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())

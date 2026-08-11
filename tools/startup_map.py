#!/usr/bin/env python3
"""Boot the title straight into a map, by editing the game's own startup config.

    tools/startup_map.py show
    tools/startup_map.py set SP_Prison_P
    tools/startup_map.py restore
    tools/startup_map.py --selftest
    tools/startup_map.py maps

WHY THIS AND NOT A RUNTIME HOOK. Reaching a gameplay scene costs about seven
minutes per side today, because both emulators have to boot and be driven
through the menus by a scripted pad walk -- and only the one scene that walk ends
at is reachable at all. A hook inside our runtime would fix that for US and not
for the ORACLE: Xenia cannot be told to call a guest function, so a comparison
built on one would have a fast side and a slow side.

The startup map is not a runtime decision. It is `[URL] LocalMap` in the title's
own coalesced config, which lives in the extracted game tree that BOTH emulators
read. Changing it there needs no hook on either side, and both arms boot into the
same level by the same mechanism the title uses for its own front end.

THE CONTAINER. WarGame/Config/Xenon/Cooked/Coalesced.ini is UE3's coalesced
config: a big-endian int32 count of STRINGS, then that many length-prefixed
NUL-terminated strings, read in (filename, contents) pairs. The count is the
string count, not the file count -- this file says 50 and holds 25 inis, and
reading it as a file count runs off the end. Every offset is checked, and the
parse must consume the file EXACTLY or this refuses.

TWO INIS CARRY THE URL: WarEngine.ini and Xe-WarEngine.ini, the Xenon override.
Both are written, because which one wins is the engine's business and setting
only one leaves the answer to a rule this tool does not know.
"""
import argparse
import shutil
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_CONFIG = "WarGame/Config/Xenon/Cooked/Coalesced.ini"
# The keys the engine reads for the startup level. Map is what the URL resolves
# to and LocalMap is the standalone-play default; the title sets both, so this
# sets both.
URL_KEYS = ("Map", "LocalMap")


def parse(data):
    """-> [(name, body)], or raises. Consumes the whole file or refuses."""
    if len(data) < 4:
        raise ValueError(f"{len(data)} bytes is too short to be a coalesced config")
    count = struct.unpack_from(">i", data, 0)[0]
    off = 4
    strings = []
    while off < len(data):
        if off + 4 > len(data):
            raise ValueError(f"a length prefix runs past the end at offset {off}")
        n = struct.unpack_from(">i", data, off)[0]
        off += 4
        if n < 0 or off + n > len(data):
            raise ValueError(f"string length {n} at offset {off - 4} does not fit")
        strings.append(data[off:off + n])
        off += n
    if off != len(data):
        raise ValueError(f"parse ended at {off} of {len(data)} -- not an exact fit")
    if len(strings) != count:
        raise ValueError(f"header says {count} strings, found {len(strings)}")
    if len(strings) % 2:
        raise ValueError(f"{len(strings)} strings is odd, so they are not "
                         f"(name, contents) pairs")
    return [(strings[i].rstrip(b"\x00").decode("latin1"),
             strings[i + 1].rstrip(b"\x00").decode("latin1"))
            for i in range(0, len(strings), 2)]


def build(files):
    """The inverse of parse(). Byte-identical for an unmodified round trip."""
    out = bytearray()
    out += struct.pack(">i", len(files) * 2)
    for name, body in files:
        for s in (name, body):
            raw = s.encode("latin1") + b"\x00"
            out += struct.pack(">i", len(raw))
            out += raw
    return bytes(out)


def url_section(body):
    """-> (start, end) of the [URL] section's body, or None."""
    key = "[URL]"
    i = body.find(key)
    if i < 0:
        return None
    start = i + len(key)
    nxt = body.find("[", start)
    return (start, len(body) if nxt < 0 else nxt)


def read_url(body):
    span = url_section(body)
    if span is None:
        return {}
    out = {}
    for line in body[span[0]:span[1]].splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            out.setdefault(k.strip(), v.strip())
    return out


def set_url(body, values):
    """Rewrite the named keys inside [URL]. Returns (body, changed_keys)."""
    span = url_section(body)
    if span is None:
        return body, []
    head, section, tail = body[:span[0]], body[span[0]:span[1]], body[span[1]:]
    changed = []
    lines = section.split("\n")
    for i, line in enumerate(lines):
        stripped = line.strip()
        if "=" not in stripped:
            continue
        k = stripped.split("=", 1)[0].strip()
        if k in values:
            # Preserve the line ending the file uses -- these are \r\n, and
            # rewriting one as \n would shift every offset after it.
            eol = "\r" if line.endswith("\r") else ""
            lines[i] = f"{k}={values[k]}{eol}"
            changed.append(k)
    return head + "\n".join(lines) + tail, changed


def config_path(args):
    game = Path(args.game_dir) if args.game_dir else REPO / "scratch" / "game"
    return game / DEFAULT_CONFIG


def backup_path(p):
    return p.with_suffix(p.suffix + ".orig")


def cmd_show(path):
    files = parse(path.read_bytes())
    found = 0
    for name, body in files:
        url = read_url(body)
        if not any(k in url for k in URL_KEYS):
            continue
        found += 1
        short = name.split("\\")[-1]
        print(f"  {short:<20} " + "  ".join(f"{k}={url.get(k, '-')}" for k in URL_KEYS))
    # ALWAYS says how many it looked at: "no URL section anywhere" and "this
    # tool did not look" are the same silence otherwise.
    print(f"{found} of {len(files)} config file(s) carry a [URL] startup map")
    b = backup_path(path)
    print(f"backup: {'present at ' + str(b) if b.exists() else 'NONE -- set has not run yet'}")
    return 0


def cmd_set(path, mapname):
    data = path.read_bytes()
    files = parse(data)
    # Prove the writer is faithful on THIS file before letting it modify it. A
    # writer that drops a NUL or mis-sizes a prefix would still produce a
    # plausible file, and the title would fail somewhere unrelated.
    if build(files) != data:
        print("REFUSING: this tool cannot round-trip the config byte for byte, "
              "so it must not rewrite it. Nothing was written.")
        return 2
    b = backup_path(path)
    if not b.exists():
        shutil.copy2(path, b)
        print(f"backed up the original to {b}")
    out, touched = [], 0
    for name, body in files:
        body2, changed = set_url(body, {k: mapname for k in URL_KEYS})
        if changed:
            touched += 1
            print(f"  {name.split(chr(92))[-1]:<20} set {', '.join(changed)} = {mapname}")
        out.append((name, body2))
    if not touched:
        print("REFUSING: no [URL] section carried a startup map key, so nothing "
              "would have changed. Nothing was written.")
        return 2
    path.write_bytes(build(out))
    # Read it back through the parser rather than trusting the write.
    check = parse(path.read_bytes())
    for name, body in check:
        url = read_url(body)
        for k in URL_KEYS:
            if k in url and url[k] != mapname:
                print(f"REFUSING TO REPORT SUCCESS: {name} still has {k}={url[k]}")
                return 2
    print(f"startup map is now {mapname} in {touched} config file(s); "
          f"both emulators read this tree, so both boot into it")
    return 0


def cmd_restore(path):
    b = backup_path(path)
    if not b.exists():
        print(f"nothing to restore: {b} does not exist")
        return 1
    shutil.copy2(b, path)
    print(f"restored {path} from {b}")
    return cmd_show(path)


def cmd_maps(args):
    game = Path(args.game_dir) if args.game_dir else REPO / "scratch" / "game"
    cooked = game / "WarGame" / "CookedXenon"
    if not cooked.is_dir():
        print(f"REFUSING: {cooked} does not exist, so this searched NOTHING.")
        return 1
    # The persistent levels: UE3 cooks one <name>_P package per playable map and
    # streams the rest in. Anything else is a sublevel and is not loadable on
    # its own.
    maps = sorted(p.stem for p in cooked.glob("*.xxx")
                  if p.stem.lower().endswith("_p"))
    for m in maps:
        print(f"  {m}")
    print(f"{len(maps)} persistent map(s) of {len(list(cooked.glob('*.xxx')))} "
          f"packages in {cooked}")
    return 0


def selftest():
    """Round-trip the REAL config unchanged and require byte identity.

    This is the check that matters: a writer that corrupts lengths still
    produces a file, and 'the game did not boot' is a very expensive way to
    find out. Also checks that setting a value is visible through the parser,
    so a no-op writer cannot pass either.
    """
    path = REPO / "scratch" / "game" / DEFAULT_CONFIG
    if not path.exists():
        print(f"SELFTEST FAIL: {path} does not exist, so NOTHING was tested.")
        return 1
    data = path.read_bytes()
    files = parse(data)
    ok = build(files) == data
    print(f"selftest: unchanged round trip is byte-identical: {ok} (expected True)")
    before = [read_url(b).get("LocalMap") for _, b in files if read_url(b)]
    changed = [(n, set_url(b, {k: "SELFTEST_MAP" for k in URL_KEYS})[0])
               for n, b in files]
    after = [read_url(b).get("LocalMap") for _, b in changed if read_url(b)]
    fired = any(a == "SELFTEST_MAP" for a in after) and before != after
    print(f"selftest: setting a map is visible through the parser: {fired} "
          f"(expected True)")
    # And the modified image must still round-trip, or `set` would write a file
    # this tool could not read back.
    reread = parse(build(changed))
    rt = any(read_url(b).get("LocalMap") == "SELFTEST_MAP" for _, b in reread)
    print(f"selftest: the modified config re-parses and keeps the value: {rt} "
          f"(expected True)")
    good = ok and fired and rt
    print("SELFTEST PASS" if good else "SELFTEST FAIL: do not let this tool write")
    return 0 if good else 1


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", nargs="?", default="show",
                    choices=["show", "set", "restore", "maps"])
    ap.add_argument("mapname", nargs="?")
    ap.add_argument("--game-dir", default=None,
                    help="the extracted game tree (default scratch/game)")
    ap.add_argument("--selftest", action="store_true",
                    help="prove the reader and writer round-trip before trusting them")
    args = ap.parse_args(argv[1:])
    if args.selftest:
        return selftest()
    if args.command == "maps":
        return cmd_maps(args)
    path = config_path(args)
    if not path.exists():
        print(f"REFUSING: {path} does not exist. Nothing was read.")
        return 1
    try:
        if args.command == "show":
            return cmd_show(path)
        if args.command == "restore":
            return cmd_restore(path)
        if args.command == "set":
            if not args.mapname:
                print("REFUSING: `set` needs a map name. Try `maps` for the list.")
                return 2
            return cmd_set(path, args.mapname)
    except ValueError as exc:
        print(f"REFUSING: {exc}. Nothing was written.")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

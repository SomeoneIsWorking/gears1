#!/usr/bin/env python3
"""catalog.py — a tiny, dependency-free issue/finding catalog.

A symptom-keyed, greppable registry of issues, findings, root causes, and DEAD
ENDS for a codebase. One Markdown file per entry under a catalog dir (default
`docs/issues/`), each with a small frontmatter block so it stays human-readable
and `grep`-able even without this tool. Consult before re-deriving; write both
wins and dead ends.

Zero dependencies (Python 3.8+ stdlib only). Run from a project root:

    catalog.py add "shader colors wrong" --symptom "creature meshes render grey" \
        --tags render,material --status open
    catalog.py search "grey enemy"          # rank by symptom/title/tag/body match
    catalog.py list --status open
    catalog.py show 7
    catalog.py resolve 7 "p_Masks.B is the emissive mask, not zone C"
    catalog.py deadend 7 "tried tinting per-vertex -> wrong, blend is in the DXBC"

Storage: docs/issues/0007-shader-colors-wrong.md  (override with --dir or
$CATALOG_DIR). Entries are plain Markdown; edit them by hand freely.
"""
import argparse
import datetime
import os
import re
import sys
import textwrap

DEFAULT_DIR = os.environ.get("CATALOG_DIR", "docs/issues")
STATUSES = ("open", "investigating", "resolved", "wontfix", "dead-end")
FM_RE = re.compile(r"^---\n(.*?)\n---\n(.*)$", re.DOTALL)


def _now():
    return datetime.date.today().isoformat()


def _slug(text, maxlen=48):
    s = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")
    return (s[:maxlen].rstrip("-")) or "issue"


def _parse(path):
    """Parse an entry file -> dict(meta..., body=..., path=...). Tolerant."""
    with open(path, encoding="utf-8") as f:
        raw = f.read()
    meta, body = {}, raw
    m = FM_RE.match(raw)
    if m:
        body = m.group(2)
        for line in m.group(1).splitlines():
            if ":" in line:
                k, _, v = line.partition(":")
                meta[k.strip()] = v.strip()
    try:
        meta["id_num"] = int(meta.get("id", "0"))
    except ValueError:
        meta["id_num"] = 0
    meta["tags_list"] = [t for t in re.split(r"[,\s]+", meta.get("tags", "")) if t]
    meta["path"] = path
    meta["body"] = body.strip()
    return meta


def _load_all(cdir):
    if not os.path.isdir(cdir):
        return []
    out = []
    for name in sorted(os.listdir(cdir)):
        if name.endswith(".md") and name.lower() != "readme.md":
            out.append(_parse(os.path.join(cdir, name)))
    return out


def _find(cdir, ident):
    ident = str(ident).lstrip("#")
    for e in _load_all(cdir):
        if str(e["id_num"]) == ident or e.get("id") == ident:
            return e
    return None


def _write(cdir, meta, body):
    order = ["id", "title", "status", "symptom", "tags", "created", "updated"]
    keys = order + [k for k in meta if k not in order and not k.endswith("_list")
                    and k not in ("id_num", "path", "body")]
    fm = "\n".join(f"{k}: {meta[k]}" for k in keys if k in meta and meta[k] != "")
    path = meta["path"]
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"---\n{fm}\n---\n\n{body.strip()}\n")
    return path


def cmd_add(args):
    cdir = args.dir
    os.makedirs(cdir, exist_ok=True)
    nid = max([e["id_num"] for e in _load_all(cdir)], default=0) + 1
    fname = f"{nid:04d}-{_slug(args.title)}.md"
    path = os.path.join(cdir, fname)
    meta = {
        "id": str(nid), "title": args.title, "status": args.status,
        "symptom": args.symptom or "", "tags": ",".join(args.tags or []),
        "created": _now(), "updated": _now(), "path": path,
    }
    body = args.body or textwrap.dedent("""\
        ## Root cause


        ## What was tried / dead ends


        ## Resolution
        """)
    _write(cdir, meta, body)
    print(f"created #{nid}: {path}")


def _append_note(cdir, ident, heading, note, new_status=None):
    e = _find(cdir, ident)
    if not e:
        sys.exit(f"no entry #{ident} in {cdir}")
    stamp = _now()
    body = e["body"] + f"\n\n### {heading} ({stamp})\n{note}\n"
    if new_status:
        e["status"] = new_status
    e["updated"] = stamp
    _write(cdir, e, body)
    print(f"updated #{e['id_num']} [{e['status']}]: {e['path']}")


def cmd_resolve(args):
    _append_note(args.dir, args.id, "Resolution", args.note, "resolved")


def cmd_deadend(args):
    _append_note(args.dir, args.id, "Dead end", args.note, None)


def cmd_note(args):
    _append_note(args.dir, args.id, "Note", args.note,
                 args.status if args.status else None)


def cmd_reopen(args):
    _append_note(args.dir, args.id, "Reopened", args.note or "reopened", "open")


def _score(e, terms):
    hay = {
        "symptom": e.get("symptom", "").lower(), "title": e.get("title", "").lower(),
        "tags": " ".join(e["tags_list"]).lower(), "body": e["body"].lower(),
    }
    weights = {"symptom": 5, "title": 4, "tags": 3, "body": 1}
    score = 0
    for t in terms:
        for field, text in hay.items():
            score += text.count(t) * weights[field]
    return score


def cmd_search(args):
    terms = [t.lower() for t in re.split(r"\s+", args.query.strip()) if t]
    scored = [(s, e) for e in _load_all(args.dir) if (s := _score(e, terms)) > 0]
    scored.sort(key=lambda x: (-x[0], x[1]["id_num"]))
    if not scored:
        print("(no matches)")
        return
    for score, e in scored[: args.limit]:
        _print_row(e, extra=f"score={score}")
        snip = _snippet(e, terms)
        if snip:
            print(f"      … {snip}")


def _snippet(e, terms, width=120):
    text = f"{e.get('symptom','')} {e['body']}".replace("\n", " ")
    low = text.lower()
    for t in terms:
        i = low.find(t)
        if i >= 0:
            start = max(0, i - 30)
            return text[start:start + width].strip()
    return ""


def _print_row(e, extra=""):
    tags = ",".join(e["tags_list"])
    print(f"  #{e['id_num']:<4} [{e.get('status','?'):<13}] {e.get('title','')}"
          + (f"  ({tags})" if tags else "") + (f"  {extra}" if extra else ""))


def cmd_list(args):
    entries = _load_all(args.dir)
    if args.status:
        entries = [e for e in entries if e.get("status") == args.status]
    if args.tag:
        entries = [e for e in entries if args.tag in e["tags_list"]]
    entries.sort(key=lambda e: e["id_num"])
    if not entries:
        print("(no entries)")
        return
    for e in entries:
        _print_row(e)
    print(f"\n{len(entries)} entr{'y' if len(entries)==1 else 'ies'} in {args.dir}")


def cmd_show(args):
    e = _find(args.dir, args.id)
    if not e:
        sys.exit(f"no entry #{args.id} in {args.dir}")
    with open(e["path"], encoding="utf-8") as f:
        sys.stdout.write(f.read())


def main(argv=None):
    p = argparse.ArgumentParser(description="tiny symptom-keyed issue/finding catalog")
    p.add_argument("--dir", default=DEFAULT_DIR,
                   help=f"catalog dir (default {DEFAULT_DIR}, or $CATALOG_DIR)")
    sub = p.add_subparsers(dest="cmd", required=True)

    a = sub.add_parser("add", help="create a new entry")
    a.add_argument("title")
    a.add_argument("--symptom", help="the observable symptom (search-weighted highest)")
    a.add_argument("--tags", type=lambda s: [t for t in re.split(r"[,\s]+", s) if t],
                   help="comma/space separated")
    a.add_argument("--status", choices=STATUSES, default="open")
    a.add_argument("--body", help="full body markdown (else a template is written)")
    a.set_defaults(func=cmd_add)

    s = sub.add_parser("search", help="rank entries by symptom/title/tag/body match")
    s.add_argument("query")
    s.add_argument("--limit", type=int, default=10)
    s.set_defaults(func=cmd_search)

    l = sub.add_parser("list", help="list entries")
    l.add_argument("--status", choices=STATUSES)
    l.add_argument("--tag")
    l.set_defaults(func=cmd_list)

    sh = sub.add_parser("show", help="print an entry")
    sh.add_argument("id")
    sh.set_defaults(func=cmd_show)

    for name, fn, help_ in [
        ("resolve", cmd_resolve, "mark resolved + append resolution note"),
        ("deadend", cmd_deadend, "append a dead-end note (keeps status)"),
        ("reopen", cmd_reopen, "set status open + append note"),
    ]:
        sp = sub.add_parser(name, help=help_)
        sp.add_argument("id")
        sp.add_argument("note", nargs="?" if name == "reopen" else None, default="")
        sp.set_defaults(func=fn)

    n = sub.add_parser("note", help="append a note, optionally change status")
    n.add_argument("id")
    n.add_argument("note")
    n.add_argument("--status", choices=STATUSES)
    n.set_defaults(func=cmd_note)

    args = p.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()

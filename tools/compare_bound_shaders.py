#!/usr/bin/env python3
"""Compare runtime-captured microcode against the offline container corpus.

The runtime capture (GEARS_SHADER_CAPTURE=1) writes bare Xenos microcode, taken
from the PM4 sequencer-load packets the title actually issues. The offline
corpus (tools/shader_extract.py) holds whole containers scraped from the
uncompressed parts of the cooked packages. The interesting question is how much
of what the title BINDS is in that corpus at all -- anything missing can only
have come from somewhere the offline scan cannot see (a compressed package
chunk, or microcode the driver builds).

Comparison is on the microcode bytes, which is the only thing both sides share.

    tools/compare_bound_shaders.py scratch/shaders/bound scratch/shaders/all
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shader_extract import parse, BadContainer


def corpus_ucode(directory):
    """-> (hash -> [container names], parsed_count, skipped_count).

    THE SKIP COUNT IS PART OF THE RESULT, not a log line. Every container this
    function fails to parse makes the corpus smaller, and a smaller corpus turns
    directly into more shaders reported "NOT IN CORPUS" -- which is this tool's
    headline finding. A corpus that mostly failed to parse would report almost
    everything as new microcode from somewhere unreachable, which is a
    conclusion about the title drawn from a defect in the parser. So the ratio
    is carried out and printed beside the answer.
    """
    out = {}
    parsed = skipped = 0
    for name in sorted(os.listdir(directory)):
        if not name.endswith(".bin"):
            continue
        data = open(os.path.join(directory, name), "rb").read()
        try:
            c = parse(data, 0)
        except BadContainer as e:
            print("skip %s: %s" % (name, e))
            skipped += 1
            continue
        parsed += 1
        out.setdefault(hashlib.sha1(c["ucode"]).hexdigest(), []).append(name)
    return out, parsed, skipped


def main():
    bound_dir, corpus_dir = sys.argv[1], sys.argv[2]
    corpus, parsed, skipped = corpus_ucode(corpus_dir)

    binds = {}
    manifest = os.path.join(bound_dir, "manifest.csv")
    if os.path.exists(manifest):
        for line in open(manifest).read().splitlines()[1:]:
            f = line.split(",")
            binds[f[0]] = int(f[-1])

    rows = []
    for name in sorted(os.listdir(bound_dir)):
        if not name.endswith(".ucode"):
            continue
        ucode = open(os.path.join(bound_dir, name), "rb").read()
        digest = hashlib.sha1(ucode).hexdigest()
        rows.append((binds.get(name, 0), name, len(ucode) // 12,
                     corpus.get(digest)))
    rows.sort(reverse=True)

    total = sum(r[0] for r in rows)
    known = sum(1 for r in rows if r[3])
    print("%-28s %5s %7s %8s  %s" % ("captured", "instr", "binds", "share",
                                     "offline corpus match"))
    for count, name, instrs, match in rows:
        print("%-28s %5d %7d %7.2f%%  %s"
              % (name, instrs, count, 100.0 * count / total if total else 0,
                 match[0] if match else "NOT IN CORPUS"))
    print("\n%d captured, %d in the offline corpus, %d new; %d total binds"
          % (len(rows), known, len(rows) - known, total))

    # THE DENOMINATOR, BESIDE THE ANSWER. "NOT IN CORPUS" only means "the title
    # got this from somewhere the offline scan cannot see" if the corpus was
    # actually read. Every skipped container is a shader that could have matched
    # and silently could not.
    print("corpus: %d container(s) parsed, %d skipped as unparseable, %d "
          "distinct microcode payload(s)" % (parsed, skipped, len(corpus)))
    if parsed == 0:
        print("THE CORPUS IS EMPTY: every 'NOT IN CORPUS' above is a failure to "
              "read %s, not a finding about the title." % corpus_dir)
    elif skipped:
        print("  %.1f%% of the corpus did not parse, so 'NOT IN CORPUS' is an "
              "UPPER BOUND on genuinely-new microcode." % (100.0 * skipped /
                                                           (parsed + skipped)))
    if not rows:
        print("NO CAPTURED SHADERS AT ALL in %s -- this run compared nothing. "
              "Was GEARS_SHADER_CAPTURE=1 set?" % bound_dir)
    elif total == 0:
        print("Every captured shader shows 0 binds: manifest.csv was missing or "
              "its columns moved, so the 'binds' and 'share' columns above are "
              "not measurements.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

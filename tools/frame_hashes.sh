#!/usr/bin/env bash
# "Did this change the picture?" -- render every capture and print a hash of each
# rendered frame. Run it before a refactor, run it after, diff the two.
#
# WHY A HASH AND NOT A PIXEL COUNT. The frame line's "N px non-black" is what was
# reachable before, and it is not a regression gate: a change that moves colour
# around without changing how many pixels are lit reads as identical. A hash of
# the bytes cannot do that.
#
# WHAT IT WILL NOT TELL YOU. It compares this build against another build of the
# SAME renderer. Every capture could be wrong in the same way in both runs and
# this script would report a clean match -- it certifies "unchanged", never
# "correct". It also renders one frame per capture, so it says nothing about
# anything that varies frame to frame.
#
# IT NEVER ENTERS A DIAGNOSTIC. No GEARS_DRAW_* knob is set here, so the whole
# instrumented half of the renderer -- FRAME_STEP, PIXEL_TRACE, DIAG, the dumps,
# the A/B arms -- is dead code in every run this script makes. A refactor that
# touches one of those is NOT covered by a clean report here; run the knob.
#
#   tools/frame_hashes.sh [output.txt]
set -euo pipefail
cd "$(dirname "$0")/.."

replay=scratch/build/runtime/frame_replay
shots=scratch/screenshots
out=${1:-}

[[ -x $replay ]] || { echo "build $replay first (ninja -C scratch/build frame_replay)" >&2; exit 1; }

captures=(scratch/frames/*.gfr)
allblack=()
# A GLOB THAT MATCHED NOTHING IS A FAILURE, not an empty report. Without this the
# script prints a header and exits 0, which reads exactly like "everything is
# unchanged".
(( ${#captures[@]} )) || { echo "REFUSING: no captures in scratch/frames -- this run compared NOTHING" >&2; exit 1; }

emit() { if [[ -n $out ]]; then tee -a "$out"; else cat; fi; }
[[ -n $out ]] && : >"$out"

echo "# rendered-frame hashes, ${#captures[@]} capture(s)" | emit
for c in "${captures[@]}"; do
    rm -f "$shots"/frame*.ppm
    if ! "$replay" "$c" >/dev/null 2>&1; then
        echo "$(basename "$c")	REPLAY-FAILED" | emit
        continue
    fi
    produced=$(ls "$shots"/frame*.ppm 2>/dev/null | head -1 || true)
    if [[ -z $produced ]]; then
        # No screenshot is a failure of the run, not a frame that hashes to nothing.
        echo "$(basename "$c")	NO-SCREENSHOT" | emit
        continue
    fi
    # AN ALL-BLACK FRAME IS NOT A HASH, IT IS A FAILED RENDER -- and it hashes
    # just as cleanly as a picture. Two captures that render nothing produce the
    # SAME hash and this script reported them as an ordinary match: play_v2.gfr
    # and character_auto.gfr both hashed 847b7f79e03d5c66, which is the hash of
    # 921600 black pixels. Uniform output is the classic broken-instrument tell,
    # so it is called out here rather than left for someone to notice that two
    # unrelated captures agree.
    black=$(python3 - "$produced" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
# P6 header: magic, dimensions, maxval -- three whitespace-separated fields
# after "P6", then one byte of whitespace, then the pixels.
i, fields = 2, 0
while fields < 3 and i < len(d):
    while i < len(d) and d[i] in b' \t\r\n': i += 1
    while i < len(d) and d[i] not in b' \t\r\n': i += 1
    fields += 1
i += 1
print("BLACK" if fields == 3 and not any(d[i:]) else "")
PY
)
    if [[ $black == BLACK ]]; then
        echo "$(basename "$c")	$(sha256sum "$produced" | cut -c1-16)	ALL-BLACK" | emit
        allblack+=("$(basename "$c")")
    else
        echo "$(basename "$c")	$(sha256sum "$produced" | cut -c1-16)" | emit
    fi
done

# Said once at the end as well, because a per-row tag scrolls past in a
# sixteen-capture run and the count is the thing worth reacting to.
if (( ${#allblack[@]} )); then
    echo "# ${#allblack[@]} of ${#captures[@]} captures rendered a COMPLETELY BLACK frame:" \
         "${allblack[*]}" | emit
    echo "# Their hashes are stable and comparable, so this script's" \
         "unchanged/changed answer still holds for them -- but a black frame is" \
         "a rendering failure, not a picture, and two black captures always" \
         "match each other." | emit
fi

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
#   tools/frame_hashes.sh [output.txt]
set -euo pipefail
cd "$(dirname "$0")/.."

replay=scratch/build/runtime/frame_replay
shots=scratch/screenshots
out=${1:-}

[[ -x $replay ]] || { echo "build $replay first (ninja -C scratch/build frame_replay)" >&2; exit 1; }

captures=(scratch/frames/*.gfr)
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
    echo "$(basename "$c")	$(sha256sum "$produced" | cut -c1-16)" | emit
done

#!/usr/bin/env bash
# The acceptance gate for a native pass: render a captured frame twice -- once
# through the title's translated microcode, once through our own shader -- and
# require the two to match.
#
# WHY THIS IS A SCRIPT. Run by hand it has a trap that already fired once: the
# replay names its screenshot after the frame number, so a capture that writes
# frame_00600.ppm leaves an earlier frame.ppm in place, and copying the wrong file
# compares a frame against ITSELF and reports a perfect match. This script deletes
# the output before every arm and fails if none reappears, so "match" cannot be
# produced by a stale file.
#
# It also runs a NEGATIVE CONTROL when a second capture is available: a comparison
# that has never been shown reporting a difference is not evidence of a match.
#
#   tools/verify_native_pass.sh [capture.gfr] [control.gfr]
set -euo pipefail
cd "$(dirname "$0")/.."

capture=${1:-scratch/frames/boot150.gfr}
control=${2:-scratch/frames/act1.gfr}
replay=scratch/build/runtime/frame_replay
out=scratch/ab
shots=scratch/screenshots

[[ -x $replay ]]   || { echo "build $replay first (ninja -C scratch/build frame_replay)" >&2; exit 1; }
[[ -f $capture ]]  || { echo "no capture at $capture" >&2; exit 1; }
mkdir -p "$out"

# Render one arm, insisting on a FRESH screenshot.
arm() { # arm <NATIVE_PASSES value> <capture> <destination>
    rm -f "$shots"/frame*.ppm
    GEARS_NATIVE_PASSES=$1 "$replay" "$2" >/dev/null 2>&1 || {
        echo "REFUSING to compare: the replay of $2 failed with GEARS_NATIVE_PASSES=$1" >&2
        exit 1; }
    local produced
    produced=$(ls "$shots"/frame*.ppm 2>/dev/null | head -1 || true)
    [[ -n $produced ]] || {
        echo "REFUSING to compare: the replay of $2 wrote no screenshot. It rendered" >&2
        echo "NOTHING -- this is a failure, not an empty result." >&2
        exit 1; }
    cp "$produced" "$3"
}

echo "== translated microcode vs our own shader, on $capture =="
arm 0 "$capture" "$out/xlate.ppm"
arm 1 "$capture" "$out/native.ppm"
status=0
tools/compare_frames.py "$out/xlate.ppm" "$out/native.ppm" || status=$?

if [[ -f $control ]]; then
    echo
    echo "== negative control: $control must NOT match =="
    arm 0 "$control" "$out/control.ppm"
    if tools/compare_frames.py "$out/xlate.ppm" "$out/control.ppm" >/dev/null 2>&1; then
        echo "INSTRUMENT FAILURE: the comparison calls two different captures" \
             "identical. It cannot be trusted to have found a match above." >&2
        exit 1
    fi
    echo "the comparison reports a difference on a frame that IS different -- so a" \
         "match above is a result, not a tautology"
else
    echo
    echo "NO NEGATIVE CONTROL: $control is missing, so this run never demonstrated" \
         "that the comparison can report a difference at all. Capture a second" \
         "frame before believing the match above."
fi
exit $status

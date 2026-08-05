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

# The v2 captures, which carry the front-buffer address. A v1 capture still gives
# a valid A/B -- both arms replay identical input -- but it cannot answer anything
# about the presented buffer, so it is not the default any more.
capture=${1:-scratch/frames/act1_v2.gfr}
control=${2:-scratch/frames/play_v2.gfr}
replay=scratch/build/runtime/frame_replay
out=scratch/ab
shots=scratch/screenshots

[[ -x $replay ]]   || { echo "build $replay first (ninja -C scratch/build frame_replay)" >&2; exit 1; }
[[ -f $capture ]]  || { echo "no capture at $capture" >&2; exit 1; }
mkdir -p "$out"

# Render one arm, insisting on a FRESH screenshot.
arm() { # arm <NATIVE_PASSES value> <capture> <destination>
    rm -f "$shots"/frame*.ppm
    GEARS_NATIVE_PASSES=$1 "$replay" "$2" >"$out/arm$1.log" 2>&1 || {
        echo "REFUSING to compare: the replay of $2 failed with GEARS_NATIVE_PASSES=$1" >&2
        exit 1; }
    # THE CHECK THAT MAKES THE MATCH MEAN ANYTHING. If no pass was substituted,
    # the two arms ran the same shaders and a perfect match is a tautology -- and
    # that is the silent failure mode of this whole gate, because a pass whose
    # hash never appears in the capture behaves exactly like a pass that works.
    if [[ $1 == 1 ]] && ! grep -q "is rendering natively" "$out/arm1.log"; then
        echo "REFUSING to report a result: NO native pass was substituted in $2." >&2
        echo "Both arms ran the title's translated shaders, so they match for a" >&2
        echo "reason that has nothing to do with the shader under test. The roster" >&2
        echo "line from that run was:" >&2
        grep "^\[native\]" "$out/arm1.log" >&2 || echo "  (none -- is the build current?)" >&2
        exit 1
    fi
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
grep "is rendering natively" "$out/arm1.log" | sed 's/^/  substituted: /'
status=0
tools/compare_frames.py "$out/xlate.ppm" "$out/native.ppm" || status=$?

# THE INTERFACE ARM. A pixel comparison audits a RESULT, not an interface: two of
# this project's native passes were bit-exact for two sessions while declaring
# their sampled images as texture2D against VK_IMAGE_VIEW_TYPE_2D_ARRAY
# descriptors. The driver tolerates that and samples layer 0, so the comparison
# above could never have reported it -- Vulkan validation reports it in one line
# per draw (catalog #72). Necessary AND sufficient needs both arms.
echo
echo "== interface: Vulkan validation, native passes on =="
rm -f "$shots"/frame*.ppm
GEARS_DRAW_VALIDATE=1 GEARS_NATIVE_PASSES=1 "$replay" "$capture" \
    >"$out/validate.log" 2>&1 || {
    echo "REFUSING to report: the validation run of $capture failed" >&2; exit 1; }
# Count them rather than grepping for a pattern that might match nothing because
# the run died early: a zero that cannot be distinguished from "never ran" is the
# failure mode this whole script exists to avoid.
draws_seen=$(grep -c "is rendering natively" "$out/validate.log" || true)
if [[ $draws_seen -eq 0 ]]; then
    echo "REFUSING to report an interface result: no native pass was substituted" >&2
    echo "in the validation run, so validation had nothing of ours to check." >&2
    exit 1
fi
iface=$(grep -c "VkImageViewType\|OpTypeImage" "$out/validate.log" || true)
echo "  $draws_seen native substitutions under validation;" \
     "$iface descriptor/shader-interface warnings"
if [[ $iface -ne 0 ]]; then
    echo
    echo "INTERFACE MISMATCH: the pixels match and the interface does not. The" >&2
    echo "native module does not describe the descriptors it is handed:" >&2
    grep "VkImageViewType\|OpTypeImage" "$out/validate.log" | sort -u | head -5 >&2
    status=1
else
    echo "  the native modules describe the descriptors they are handed -- so the" \
         "match above is a match on the interface as well as on the pixels"
fi

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

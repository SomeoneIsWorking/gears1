#!/usr/bin/env bash
# "Is the renderer still valid Vulkan?" -- replay EVERY capture with the
# validation layer on and report the VUIDs each one raises.
#
# WHY THIS EXISTS AS A SEPARATE TOOL. tools/frame_hashes.sh is the routine gate
# and it sets no GEARS_DRAW_* knob, so the validation layer is never loaded in
# it. "Vulkan validation clean" therefore drifted for weeks while every gate
# stayed green, and hid seven VUIDs: a depth image cleared without TRANSFER_DST,
# two SPIR-V constructs the resolve shaders emitted at a version that forbids
# them, and every alpha-tested pixel shader declaring a device feature the
# runtime never enabled (catalog.py show 75, 76).
#
# WHY IT RUNS ALL OF THEM. The first investigation ran ONE capture, act1_v2, and
# it came back clean -- because act1_v2 happens to contain no alpha-tested pixel
# shader. Five of the other seven were not clean. A single capture is not a
# check; the whole set is barely one.
#
# The point-list PointSize warning is EXPECTED and known: the guest draws point
# lists whose vertex shader writes no PointSize. It is listed rather than
# filtered, so a run cannot look clean by having its only finding hidden.
#
#   tools/validate_all.sh [output.txt]
set -euo pipefail
cd "$(dirname "$0")/.."

replay=scratch/build/runtime/frame_replay
out=${1:-}

[[ -x $replay ]] || { echo "build $replay first (ninja -C scratch/build frame_replay)" >&2; exit 1; }

captures=(scratch/frames/*.gfr)
# A GLOB THAT MATCHED NOTHING IS A FAILURE, not an empty report -- otherwise
# this prints a header, exits 0, and reads exactly like "everything is valid".
(( ${#captures[@]} )) || { echo "REFUSING: no captures in scratch/frames -- this run validated NOTHING" >&2; exit 1; }

emit() { if [[ -n $out ]]; then tee -a "$out"; else cat; fi; }
[[ -n $out ]] && : >"$out"

known=VUID-VkGraphicsPipelineCreateInfo-topology-08773   # the PointSize warning

echo "# vulkan validation, ${#captures[@]} capture(s); $known is the known point-list warning" | emit
unexpected=0
for c in "${captures[@]}"; do
    # `|| true` on the grep: a capture that raises NO VUID at all makes grep
    # exit 1, and under `set -o pipefail` that killed this script four captures
    # in -- reporting a PASS for the ones it had reached and never saying it had
    # stopped. The clean case was the untested one.
    vuids=$(GEARS_DRAW_VALIDATE=1 "$replay" "$c" 2>&1 |
            { grep -oE 'VUID-[A-Za-z0-9-]+' || true; } | sort | uniq -c |
            awk '{printf "%sx%s ", $2, $1}')
    novel=$(printf '%s' "$vuids" | tr ' ' '\n' | grep -v "^$known" | grep -v '^$' || true)
    if [[ -n $novel ]]; then
        unexpected=1
        printf '%s\tUNEXPECTED\t%s\n' "$(basename "$c")" "$vuids" | emit
    else
        printf '%s\tok\t%s\n' "$(basename "$c")" "${vuids:-none}" | emit
    fi
done

if (( unexpected )); then
    echo "# FAIL: at least one capture raised a VUID that is not the known warning" | emit
    exit 1
fi
echo "# PASS: every capture raised only $known" | emit

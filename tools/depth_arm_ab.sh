#!/bin/sh
# THE TWO DEPTH MODELS, ON THE SAME GAME MOMENT.
#
# Every comparison of GEARS_DRAW_SPLIT_DEPTH against the shared image so far has
# been across DIFFERENT moments, and the pass being judged is one whose content
# varies between them: a flatness census of 106 mask resolves over 30 captures
# finds the second mask resolving to a flat 1.0 in six and a flat 0.0 in three,
# spanning BOTH arms. So "the split gives a flat mask #1" was never a statement
# about the split -- it is what that pass does at certain moments either way,
# and it is the entire reason the knob is off (docs/knobs.md, corrected).
#
# This holds the moment fixed. It reuses an EXISTING pair's frozen camera and
# its console dumps, and runs only OUR side, twice:
#
#   tools/depth_arm_ab.sh <existing-pair-dir> [seconds] [vs-hash]
#
# No oracle run: the console half of the named pair is the reference for both
# arms, so the two arms are scored against the SAME console frame, and any
# difference between them is the depth model and nothing else.
#
# WHAT A NULL RESULT LOOKS LIKE, written before the run so it cannot be read as
# a pass: if both arms produce the same mask flatness and correlations within
# this pass's temporal yardstick, this says the two models are INDISTINGUISHABLE
# on this moment -- not that either is right. One moment cannot settle a model;
# it can only remove the one piece of evidence that is currently keeping the
# correct-in-principle model switched off.
set -e

PAIR="${1:?usage: depth_arm_ab.sh <existing-pair-dir> [seconds] [vs-hash]}"
SECONDS_TO_RUN="${2:-600}"
VS_HASH="${3:-f3e9368c1bb68ecc}"
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$HERE/.." && pwd)
. "$REPO/tools/env.sh"
# env.sh only exports what .env provides; the runtime and game paths are
# derived the same way tools/camera_pair.sh derives them, and BOTH are
# checked here rather than letting env run "" and report a missing file.
GAME_DIR="${GEARS_GAME_DIR:-$REPO/scratch/game}"
RUNTIME="${GEARS_BUILD_DIR:-$REPO/scratch/build}/runtime/gears1"
[ -x "$RUNTIME" ] || { echo "REFUSING: $RUNTIME is not an executable \
runtime. NOTHING was run." >&2; exit 2; }
[ -f "$GAME_DIR/default.xex" ] || { echo "REFUSING: no default.xex under \
$GAME_DIR. NOTHING was run." >&2; exit 2; }

# provenance.py freezes the camera in under a FIXED name and records its
# original name in PROVENANCE.json; the frozen copy is what a replay must use,
# because the original path is exactly what a later oracle run overwrites (C042).
FROZEN="$PAIR/ours/camera.txt"
[ -f "$FROZEN" ] || { echo "REFUSING: no frozen camera at $FROZEN. That pair \
cannot be replayed and NOTHING was run." >&2; exit 2; }
[ -d "$PAIR/theirs" ] || { echo "REFUSING: no console half at $PAIR/theirs. \
There is nothing to score either arm against; NOTHING was run." >&2; exit 2; }
grep -q "c\[230\]" "$FROZEN" || { echo "REFUSING: $FROZEN carries no c[230] \
row, so it cannot gate the camera. NOTHING was run." >&2; exit 2; }

# translation fraction : rotation absolute -- the same pair the original
# capture was gated with, read from its provenance so the replay cannot be
# gated more loosely than the capture it is being compared to.
CAMERA_NEAR="${GEARS_CAMERA_NEAR:-$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['notes'].get('camera_near','0.013:0.01'))" "$PAIR/ours/PROVENANCE.json" 2>/dev/null || echo 0.013:0.01)}"
OUT="$PAIR/ab"
rm -rf "$OUT"; mkdir -p "$OUT"

run_arm() {
    arm="$1"; split="$2"
    d="$OUT/$arm"; mkdir -p "$d"
    # "default" means the variable is UNSET, which is the only way to test what
    # the SHIPPED build does. Setting it to anything -- even to the value the
    # default is supposed to have -- tests the knob and not the default, and a
    # default that silently disagrees with its documentation is exactly what
    # this arm exists to catch.
    if [ "$split" = "default" ]; then
        echo "== arm 'default' (GEARS_DRAW_SPLIT_DEPTH UNSET -- what ships) =="
        unset GEARS_DRAW_SPLIT_DEPTH
    else
        echo "== arm '$arm' (GEARS_DRAW_SPLIT_DEPTH=$split) =="
        GEARS_DRAW_SPLIT_DEPTH="$split"
        export GEARS_DRAW_SPLIT_DEPTH
    fi
    GEARS_NO_WINDOW=1 \
    GEARS_DRAW_FRAME_MIN_DRAWS="${GEARS_LAYER_MIN_DRAWS:-600}" \
    GEARS_DRAW_FRAME_AFTER_GAMEPLAY=0 \
    GEARS_DRAW_FRAME_COUNT=1 \
    GEARS_DRAW_FRAME_NEEDS="$VS_HASH" \
    GEARS_DRAW_FRAME_CAMERA="$FROZEN:$CAMERA_NEAR" \
    GEARS_DRAW_RESOLVE_DUMP_EACH=1 \
    GEARS_DRAW_DIAG="$d/draws.tsv" \
    GEARS_DRAW_DIR="$d" \
        "$RUNTIME" "$GAME_DIR/default.xex" "$GAME_DIR" > "$OUT/$arm.log" 2>&1 &
    rpid=$!
    trap 'kill -9 "$rpid" 2>/dev/null || true' EXIT INT TERM
    w=0
    while [ "$w" -lt "$SECONDS_TO_RUN" ]; do
        kill -0 "$rpid" 2>/dev/null || break
        grep -q "frame screenshot written" "$OUT/$arm.log" 2>/dev/null && break
        sleep 5; w=$((w + 5))
    done
    # Kill BY PID, never by binary name -- sibling sessions run this same
    # binary and a pattern kill takes their runs down mid-gate.
    kill -TERM "$rpid" 2>/dev/null || true
    g=0; while [ "$g" -lt 20 ] && kill -0 "$rpid" 2>/dev/null; do sleep 1; g=$((g+1)); done
    kill -9 "$rpid" 2>/dev/null || true
    wait "$rpid" 2>/dev/null || true
    trap - EXIT INT TERM

    # A DEVICE LOSS ENDS EVERYTHING, including the arms that have not run yet.
    if grep -qi "DEVICE_LOST\|Graphics device lost" "$OUT/$arm.log"; then
        echo "ARM '$arm' LOST THE VULKAN DEVICE. No arm is retried and the \
comparison is abandoned -- a second GPU run into a card the kernel is resetting \
is how a session loses the desktop." >&2
        exit 3
    fi
    grep -E "CAMERA MATCHED|held for the CAMERA" "$OUT/$arm.log" | tail -1
    grep -q "CAMERA MATCHED" "$OUT/$arm.log" || {
        echo "ARM '$arm' NEVER MATCHED THE CAMERA in ${SECONDS_TO_RUN}s, so it \
captured a DIFFERENT moment from the other arms and is not comparable to them. \
This is not a result about the depth model." >&2
        exit 4
    }
}

run_arm shared 0
run_arm split 1
# THE NOISE FLOOR. The two arms are separate RUNS, so every difference
# between them carries run-to-run variation as well as the depth model,
# and without this control a small difference cannot be told from either.
# This is the SHARED arm a second time: whatever it differs from the first
# shared run by is what two identical configurations cost, and no
# arm-vs-arm difference smaller than that means anything.
run_arm control 0
# WHAT ACTUALLY SHIPS. Every other arm sets the variable, so none of them can
# tell whether the DEFAULT selects the model its documentation claims.
run_arm default default

echo
echo "== the two arms, same camera, same console reference =="
python3 "$HERE/depth_arm_ab.py" --pair "$PAIR" --ab "$OUT"

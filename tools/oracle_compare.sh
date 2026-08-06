#!/bin/sh
# Drive OUR renderer and the Xenia oracle from the SAME scripted walk, headless,
# and put their frames side by side.
#
# WHAT THIS CAN AND CANNOT SETTLE -- read before quoting a result.
#
# The two runs are separate emulations of the same game from the same boot, fed
# the same button presses at the same wall-clock times. They are NOT frame
# synchronised and cannot be: they run at different speeds, load at different
# rates, and the title's own timing is not deterministic across them. So:
#
#   * A PIXEL METRIC BETWEEN THEM IS MEANINGLESS. Two frames a third of a second
#     apart in a moving scene differ everywhere, and `compare_frames.py` would
#     report a large number that says nothing about either renderer. This script
#     therefore does NOT print one.
#   * What it produces is a filmstrip from each side at matching wall-clock
#     offsets, for a human or a later tool to compare on the things that survive
#     a small time offset: exposure, colour, whether geometry and lighting are
#     there at all, whether one side is missing a pass.
#
# For a pixel-exact comparison you need both renderers driven from the SAME
# captured frame. That route is currently blocked: the trace dump tool renders
# even Xenia's own traces black (instrument I013, distrusted).
#
#   tools/oracle_compare.sh [seconds] [interval]
#
# Outputs under scratch/oracle/compare/{ours,theirs}/ plus a manifest.
set -eu

SECONDS_TO_RUN="${1:-240}"
INTERVAL="${2:-30}"

REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT="$REPO/scratch/oracle/compare"
ISO="${GEARS_ISO:-}"
GAME_DIR="${GEARS_GAME_DIR:-$REPO/scratch/game}"
ORACLE="$REPO/scratch/oracle/oracle-build/xenia_oracle"
RUNTIME="${GEARS_BUILD_DIR:-$REPO/scratch/build}/runtime/gears1"

# The SAME walk on both sides, expressed in each one's own notation: START once
# the title screen is up, then A repeatedly. Spamming A is what gets this title
# into gameplay; a precisely timed menu walk does not survive the two runs
# reaching each screen at different times.
ORACLE_INPUT="START@25+8,A@30+2"
ours_script() {
    # runtime notation: "<ms>:<button>,<ms>:" with an empty button releasing.
    printf '25000:START,25300:'
    t=30000
    while [ "$t" -lt $((SECONDS_TO_RUN * 1000)) ]; do
        printf ',%s:A,%s:' "$t" "$((t + 150))"
        t=$((t + 2000))
    done
}

for f in "$ORACLE" "$RUNTIME"; do
    [ -x "$f" ] || { echo "REFUSING: $f is not built. Nothing was run."; exit 2; }
done
# The oracle does NOT need the disc image: it boots the extracted tree exactly
# as our runtime does. This script refused without GEARS_ISO for weeks and that
# refusal, not the emulator, is what gated every "needs the disc" note in the
# catalog. GEARS_ISO is still honoured when set, because booting from the ISO
# exercises the disc path; when it is not set, fall back to the extracted XEX
# and SAY WHICH ARM RAN -- a comparison whose input source is ambiguous is not
# reproducible.
if [ -n "$ISO" ] && [ -f "$ISO" ]; then
    ORACLE_TARGET="$ISO"
    ORACLE_SOURCE="the disc image ($ISO)"
elif [ -f "$GAME_DIR/default.xex" ]; then
    ORACLE_TARGET="$GAME_DIR/default.xex"
    ORACLE_SOURCE="the extracted tree ($GAME_DIR/default.xex); GEARS_ISO unset"
else
    echo "REFUSING: neither GEARS_ISO nor $GAME_DIR/default.xex exists, so the"
    echo "oracle has nothing to boot. Nothing was run."
    exit 2
fi

rm -rf "$OUT"; mkdir -p "$OUT/ours" "$OUT/theirs"

echo "== our renderer, headless, ${SECONDS_TO_RUN}s =="
GEARS_NO_WINDOW=1 \
GEARS_INPUT_SCRIPT="$(ours_script)" \
GEARS_DRAW_FRAME_AT=1 \
GEARS_DRAW_FRAME_COUNT=0 \
GEARS_DRAW_FRAME_REPORT_EVERY=$((INTERVAL * 30)) \
GEARS_DRAW_DIR="$OUT/ours" \
    "$RUNTIME" "$GAME_DIR/default.xex" "$GAME_DIR" > "$OUT/ours.log" 2>&1 &
ours_pid=$!
# Kill by PID, never by name: other runs of the same binary may be in flight.
trap 'kill -9 "$ours_pid" 2>/dev/null || true' EXIT INT TERM
sleep "$SECONDS_TO_RUN"
kill -9 "$ours_pid" 2>/dev/null || true
wait "$ours_pid" 2>/dev/null || true
trap - EXIT INT TERM

echo "== the oracle, headless, ${SECONDS_TO_RUN}s =="
# --store_shaders=false for two reasons, both deliberate. It avoids a heap
# corruption ("double free or corruption (!prev)") that aborts the process while
# loading a shader cache written by a PREVIOUS run -- a real defect, recorded in
# the catalog, not something this script should paper over silently. And it
# removes cross-run state: a comparison whose result depends on what an earlier
# run left in a cache is not reproducible.
SDL_AUDIODRIVER=dummy "$ORACLE" \
    --store_shaders=false \
    --target="$ORACLE_TARGET" \
    --oracle_out="$OUT/theirs" \
    --oracle_seconds="$SECONDS_TO_RUN" \
    --oracle_interval="$INTERVAL" \
    --oracle_input="$ORACLE_INPUT" > "$OUT/theirs.log" 2>&1 &
theirs_pid=$!
trap 'kill -9 "$theirs_pid" 2>/dev/null || true' EXIT INT TERM
sleep $((SECONDS_TO_RUN + 20))
kill -9 "$theirs_pid" 2>/dev/null || true
wait "$theirs_pid" 2>/dev/null || true
trap - EXIT INT TERM

ours_n=$(find "$OUT/ours" -name '*.ppm' | wc -l)
theirs_n=$(find "$OUT/theirs" -name '*.png' | wc -l)
crashes=$(grep -c "CRASH DUMP" "$OUT/theirs.log" 2>/dev/null || true)

{
    echo "walk: $ORACLE_INPUT (same presses on both sides)"
    echo "oracle booted from: $ORACLE_SOURCE"
    echo "ours:   $ours_n frames in $OUT/ours"
    echo "theirs: $theirs_n frames in $OUT/theirs"
    echo "oracle guest crashes: $crashes"
    echo
    echo "These are two separate emulations at matching wall-clock offsets, NOT"
    echo "frame-synchronised. Compare scene content, exposure and whether a pass"
    echo "is missing. Do not compute a pixel metric between them."
} | tee "$OUT/manifest.txt"

# A run where either side produced nothing is a FAILED run, and says so rather
# than leaving two directories to be interpreted.
if [ "$ours_n" -eq 0 ] || [ "$theirs_n" -eq 0 ]; then
    echo "FAILED: one side produced no frames; there is nothing to compare."
    exit 1
fi

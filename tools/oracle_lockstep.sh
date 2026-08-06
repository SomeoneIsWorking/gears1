#!/bin/sh
# Drive OUR renderer and the Xenia oracle from the SAME schedule, indexed by the
# GUEST'S OWN FRAME COUNTER, and put frame N beside frame N.
#
# WHAT THIS FIXES, AND WHAT IT STILL CANNOT PROMISE -- read before quoting a
# number from it.
#
# `oracle_compare.sh` drives both sides on a WALL-CLOCK schedule and says,
# correctly, that a pixel metric between its two filmstrips is meaningless: the
# two emulations run at different speeds and load at different rates, so the
# same wall-clock offset is a different point in the game on each side. That
# limitation is not fundamental -- it is a consequence of the clock being the
# index.
#
# Both emulators count the guest's own frame boundary (VdSwap). Our runtime
# exposes it to the input script as "f<N>:" steps; the oracle exposes it as
# CommandProcessor::guest_swap_count and takes --oracle_by_frame. Driven and
# sampled by that counter, frame N is the SAME GAME MOMENT on both sides -- for
# exactly as long as the title is deterministic under identical input.
#
# THAT PROVISO IS REAL AND IT IS MEASURED, NOT ASSUMED. Determinism is not a
# property this title is known to have: catalog #44 records it failing to
# progress on roughly one run in three. So this script runs OUR side TWICE by
# default and reports whether the two runs agree frame for frame. If our own
# renderer does not reproduce itself at frame N, nothing across the two sides
# means anything at that frame either, and the report says so instead of
# printing a cross-side number that looks authoritative.
#
#   tools/oracle_lockstep.sh [frames] [interval]
#
# Outputs under scratch/oracle/lockstep/{ours,ours2,theirs}/ plus a manifest.
set -eu

. "$(dirname "$0")/env.sh"

FRAMES="${1:-2400}"
INTERVAL="${2:-300}"

REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT="$REPO/scratch/oracle/lockstep"
GAME_DIR="${GEARS_GAME_DIR:-$REPO/scratch/game}"
ORACLE="$REPO/scratch/oracle/oracle-build/xenia_oracle"
RUNTIME="${GEARS_BUILD_DIR:-$REPO/scratch/build}/runtime/gears1"

# The SAME walk, in each side's own notation, both indexed by GUEST FRAME.
# START once the title screen is up, then A repeatedly -- spamming A is what
# gets this title into gameplay, and unlike a timed menu walk it survives the
# two sides reaching each screen at different rates, which is the whole point.
OURS_SCRIPT="f150:START,f160:,f300:A,f310:,f420:A,f430:,f540:A,f550:,f660:A,f670:,f780:A,f790:,f900:A,f910:,f1020:A,f1030:,f1140:A,f1150:,f1260:A,f1270:"
THEIRS_INPUT="START@150+270,A@300+120"

for f in "$ORACLE" "$RUNTIME"; do
    [ -x "$f" ] || { echo "REFUSING: $f is not built. Nothing was run."; exit 2; }
done
if [ ! -f "$GAME_DIR/default.xex" ]; then
    echo "REFUSING: $GAME_DIR/default.xex is missing, so our runtime has"
    echo "nothing to run. Nothing was run."
    exit 2
fi
# The oracle prefers the disc when .env names one, and falls back to the same
# extracted tree our runtime uses. Which arm ran goes in the manifest, because a
# comparison whose input source is ambiguous is not reproducible.
if [ -n "${GEARS_ISO:-}" ] && [ -f "${GEARS_ISO:-}" ]; then
    ORACLE_TARGET="$GEARS_ISO"; ORACLE_SOURCE="the disc image"
else
    ORACLE_TARGET="$GAME_DIR/default.xex"; ORACLE_SOURCE="the extracted tree"
fi

rm -rf "$OUT"; mkdir -p "$OUT/ours" "$OUT/ours2" "$OUT/theirs"

# Kill by PID, never by name: other runs of the same binary may be in flight.
run_ours() {
    dir=$1; log=$2
    GEARS_NO_WINDOW=1 \
    GEARS_INPUT_SCRIPT="$OURS_SCRIPT" \
    GEARS_DRAW_FRAME_AT=1 \
    GEARS_DRAW_FRAME_COUNT=0 \
    GEARS_DRAW_FRAME_REPORT_EVERY="$INTERVAL" \
    GEARS_DRAW_DIR="$dir" \
        "$RUNTIME" "$GAME_DIR/default.xex" "$GAME_DIR" > "$log" 2>&1 &
    ours_pid=$!
    trap 'kill -9 "$ours_pid" 2>/dev/null || true' EXIT INT TERM
    # Our side has no frame-count stop, so it is bounded by wall clock -- but
    # the frames it WRITES are indexed by the guest counter, which is what the
    # comparison joins on. Generous, because being cut short loses frames at
    # the END of the walk, which are the gameplay ones.
    waited=0
    while [ "$waited" -lt 300 ]; do
        kill -0 "$ours_pid" 2>/dev/null || break
        n=$(find "$dir" -name 'frame_*.ppm' 2>/dev/null | wc -l)
        [ "$n" -ge $((FRAMES / INTERVAL)) ] && break
        sleep 5; waited=$((waited + 5))
    done
    kill -9 "$ours_pid" 2>/dev/null || true
    wait "$ours_pid" 2>/dev/null || true
    trap - EXIT INT TERM
}

echo "== our renderer, run 1 =="
run_ours "$OUT/ours" "$OUT/ours.log"
echo "== our renderer, run 2 (the determinism control) =="
run_ours "$OUT/ours2" "$OUT/ours2.log"

echo "== the oracle, frame-driven =="
SDL_AUDIODRIVER=dummy "$ORACLE" \
    --store_shaders=false \
    --target="$ORACLE_TARGET" \
    --oracle_out="$OUT/theirs" \
    --oracle_by_frame=true \
    --oracle_frames="$FRAMES" \
    --oracle_frame_interval="$INTERVAL" \
    --oracle_input="$THEIRS_INPUT" > "$OUT/theirs.log" 2>&1 &
theirs_pid=$!
trap 'kill -9 "$theirs_pid" 2>/dev/null || true' EXIT INT TERM
waited=0
while [ "$waited" -lt 420 ]; do
    kill -0 "$theirs_pid" 2>/dev/null || break
    sleep 5; waited=$((waited + 5))
done
kill -9 "$theirs_pid" 2>/dev/null || true
wait "$theirs_pid" 2>/dev/null || true
trap - EXIT INT TERM

ours_n=$(find "$OUT/ours" -name '*.ppm' | wc -l)
ours2_n=$(find "$OUT/ours2" -name '*.ppm' | wc -l)
theirs_n=$(find "$OUT/theirs" -name '*.png' | wc -l)

{
    echo "indexed by: THE GUEST'S OWN FRAME COUNTER (VdSwap), both sides"
    echo "walk (ours):   $OURS_SCRIPT"
    echo "walk (theirs): $THEIRS_INPUT"
    echo "oracle booted from: $ORACLE_SOURCE"
    echo "ours:   $ours_n frames   ours2: $ours2_n frames   theirs: $theirs_n frames"
    echo
    echo "Frame N on the two sides is the same GAME MOMENT only as far as the"
    echo "title is deterministic under identical input. ours vs ours2 is the"
    echo "control that measures exactly that -- read it BEFORE any cross-side"
    echo "number, and disregard any frame where the two runs of our own"
    echo "renderer disagree."
} | tee "$OUT/manifest.txt"

if [ "$ours_n" -eq 0 ] || [ "$theirs_n" -eq 0 ]; then
    echo "FAILED: a side produced no frames; there is nothing to compare."
    exit 1
fi
echo
echo "next: tools/frame_stats.py --diff  <ours>/frame_XXXXX.ppm  <theirs>/frame_XXXXXX.png"
echo "      tools/chroma_compare.py --ours $OUT/ours --theirs $OUT/theirs"

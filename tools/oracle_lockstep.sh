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

# Long enough to finish the walk, not just to start it. The old default of 2400
# stopped 3,750 frames before the table's last press, so every run under it
# ended mid-menu and the gameplay frames -- the only ones worth comparing --
# were never reached.
FRAMES="${1:-7500}"
INTERVAL="${2:-300}"

REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT="$REPO/scratch/oracle/lockstep"
GAME_DIR="${GEARS_GAME_DIR:-$REPO/scratch/game}"
ORACLE="$REPO/scratch/oracle/oracle-build/xenia_oracle"
RUNTIME="${GEARS_BUILD_DIR:-$REPO/scratch/build}/runtime/gears1"

# The SAME walk, in each side's own notation, both indexed by GUEST FRAME --
# GENERATED from the one table in tools/menu_walk.sh rather than written twice.
#
# The two strings that used to live here were not the same walk. Ours stopped
# pressing at f1260; the oracle's "START@150+270,A@300+120" went on pressing
# START (which is PAUSE once the level is up) and A for the entire run, and
# neither side had any movement input, so neither walked anywhere. Frame N was
# therefore not the same game moment on the two sides at any gameplay frame,
# which is the flaw behind several retracted findings on catalog #77.
. "$(dirname "$0")/menu_walk.sh"
OURS_SCRIPT=$(gears_walk_ours)
THEIRS_INPUT=$(gears_walk_theirs)
WALK_LAST=$(gears_walk_last_frame)

# A generator that produced nothing would drive neither side, and both runs
# would sit on the title screen looking like a title that ignores input.
if [ -z "$OURS_SCRIPT" ] || [ -z "$THEIRS_INPUT" ]; then
    echo "REFUSING: the walk generator produced an empty schedule for" >&2
    echo "  ours:   '$OURS_SCRIPT'" >&2
    echo "  theirs: '$THEIRS_INPUT'" >&2
    echo "Neither side would be driven. Nothing was run." >&2
    exit 2
fi
# EVERY FRAME THE TABLE NAMES MUST APPEAR ON BOTH SIDES. Not an event COUNT:
# the two notations legitimately spell the same walk with different numbers of
# tokens -- our side writes a stick re-centre as a bare release step and adds an
# explicit release after every button, while the oracle's driver releases on its
# own and spells a re-centre "LY0". Counting tokens flagged the correct walk as
# a mismatch (13 against 16). What has to hold is that the two sides act at the
# same guest frames, so that is what is checked.
#
# walk_mismatch <table> <ours> <theirs> -- prints what does not line up, or
# nothing at all when the two walks act at the same frames.
walk_mismatch() {
    _table=$1; _ours=$2; _theirs=$3
    for _entry in $_table; do
        _frame=${_entry%%:*}
        case ",$_ours," in *",f$_frame:"*) ;; *)
            printf ' frame %s missing from OUR walk;' "$_frame" ;;
        esac
        case ",$_theirs," in *"@$_frame,"*) ;; *)
            printf ' frame %s missing from THEIR walk;' "$_frame" ;;
        esac
    done
}

# PROVE THE CHECK FIRES BEFORE TRUSTING IT TO PASS. A guard that silently
# matches everything is worse than none: it certifies the drift it exists to
# catch. This feeds it a pair that IS mismatched and refuses to continue if it
# reports them equal -- which is exactly how the first version of this guard,
# comparing token counts, would have been caught flagging a correct walk.
if [ -z "$(walk_mismatch '750:START 900:A' 'f750:START,f758:' 'START@750')" ]; then
    echo "REFUSING: the walk cross-check does not fire on a walk that IS" >&2
    echo "mismatched (frame 900 present in neither side), so it cannot be" >&2
    echo "trusted to have checked anything. Nothing was run." >&2
    exit 2
fi
if [ -n "$(walk_mismatch '750:START' 'f750:START,f758:' 'START@750')" ]; then
    echo "REFUSING: the walk cross-check reports a mismatch on a walk that" >&2
    echo "matches, so it would refuse every correct run. Nothing was run." >&2
    exit 2
fi

mismatch=$(walk_mismatch "$GEARS_WALK_TABLE" "$OURS_SCRIPT" "$THEIRS_INPUT")
if [ -n "$mismatch" ]; then
    echo "REFUSING: the two notations are not the same walk --$mismatch" >&2
    echo "  table:  $GEARS_WALK_TABLE" >&2
    echo "  ours:   $OURS_SCRIPT" >&2
    echo "  theirs: $THEIRS_INPUT" >&2
    echo "Nothing was run." >&2
    exit 2
fi
echo "walk: $(printf '%s' "$GEARS_WALK_TABLE" | wc -w) events, cross-checked (the"
echo "      check was first shown to fire on a deliberately mismatched pair)"

# A run that stops before the walk's last press never reaches gameplay, and its
# filmstrip is a menu. Reported as a refusal rather than as a comparison,
# because "the two sides differ" and "neither side got there" look identical in
# the output.
if [ "$FRAMES" -le "$WALK_LAST" ]; then
    echo "REFUSING: asked for $FRAMES frames but the walk's last press is at" >&2
    echo "frame $WALK_LAST, so neither side would finish it and both filmstrips" >&2
    echo "would be menus. Ask for more than $WALK_LAST. Nothing was run." >&2
    exit 2
fi

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

# Wall-clock budgets, derived from the walk so they cannot silently become too
# short when the walk grows. Both sides present at roughly 30 Hz; the slack is
# for boot and the level load, which stall the frame counter while the clock
# runs.
OURS_TIMEOUT=$((FRAMES / 30 + 180))
THEIRS_TIMEOUT=$((FRAMES / 30 + 300))

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
    # Bounded by the walk, not by a fixed 300 s that predates it: at the ~30 Hz
    # our runtime presents, finishing a 7,500-frame walk needs 250 s of guest
    # time before any level load is counted.
    waited=0
    while [ "$waited" -lt "$OURS_TIMEOUT" ]; do
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
while [ "$waited" -lt "$THEIRS_TIMEOUT" ]; do
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
    echo "walk table:    $GEARS_WALK_TABLE"
    echo "  (ONE table in tools/menu_walk.sh; both notations below are"
    echo "   generated from it, so the two sides cannot drift apart again)"
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

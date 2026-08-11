#!/bin/sh
# Capture the SAME game moment's pass outputs from both renderers, then diff
# them layer by layer with tools/layer_compare.py.
#
# The moment is chosen BY CONTENT, identically on both sides: GEARS_LAYER_AFTER
# frames after the first frame that submits at least GEARS_LAYER_MIN_DRAWS draws.
# A loading frame carries a handful of draws and a gameplay frame several
# hundred, so that predicate names the start of gameplay without naming an
# integer; the offset walks past the fade-in, which is black on BOTH emulators
# and therefore says nothing when compared.
#
# It is NOT selected by frame index, and that is the whole point of this script.
# The level load takes a variable number of presents, so the index at which one
# run reaches gameplay is still a loading screen in the next: catalog #89 is a
# paired capture at guest frame 2920 in which both sides dumped a loading frame,
# because 2920 was gameplay only in the run the number came from. Our runtime's
# GEARS_DRAW_FRAME_AT also perturbs what it measures -- skipping the render
# until frame N changes how long the load takes.
#
# Usage: tools/layer_capture.sh [seconds] [out-dir]
set -eu

. "$(dirname "$0")/env.sh"
. "$(dirname "$0")/menu_walk.sh"

SECONDS_TO_RUN="${1:-420}"
REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT="${2:-$REPO/scratch/layercap}"
GAME_DIR="${GEARS_GAME_DIR:-$REPO/scratch/game}"
ORACLE="$REPO/scratch/oracle/oracle-build/xenia_oracle"
RUNTIME="${GEARS_BUILD_DIR:-$REPO/scratch/build}/runtime/gears1"

# A gameplay frame submits 800+ draws and the busiest menu frame under 200, so
# 400 sits in the gap rather than on either side of it. Both emulators get the
# SAME number: a threshold that differed would select different moments while
# the output still called them a pair.
: "${GEARS_LAYER_MIN_DRAWS:=400}"
# ... and then this many frames LATER. The first gameplay frame is a fade from
# black: the console's own capture of it resolves its post-chain output and its
# front buffer entirely zero, so a comparison taken there compares two black
# frames. 300 presents is about ten seconds of guest time, well past a fade.
: "${GEARS_LAYER_AFTER:=300}"

# THE WALK IS ONLY NEEDED WHEN THE TITLE BOOTS TO ITS FRONT END. If the game's
# own startup map has been pointed at a level (tools/startup_map.py set <map>),
# both emulators boot into that level directly -- they read the same extracted
# tree, so neither needs a hook and neither needs driving. Measured: gameplay at
# guest frame 572 on BOTH sides, against 3219/3222 after a seven-minute walk.
STARTUP_MAP=$(python3 "$REPO/tools/startup_map.py" show --game-dir "$GAME_DIR" \
    2>/dev/null | sed -n 's/.*LocalMap=\([^ ]*\).*/\1/p' | head -1)
case "${STARTUP_MAP:-}" in
    ""|[Ww]ar[Ss]tart*) DIRECT_BOOT=0 ;;
    *) DIRECT_BOOT=1 ;;
esac
if [ "$DIRECT_BOOT" = "1" ]; then
    echo "startup map is '$STARTUP_MAP', so neither side is driven: both boot"
    echo "into that level from the game's own config."
    OURS_SCRIPT=""
    THEIRS_INPUT=""
    # The oracle REFUSES a frame-driven run with no input schedule, because
    # that is normally a filmstrip of the title screen. Here it is not: the
    # title boots into a level from its own config and there is nothing to
    # press. This flag is that claim, made explicitly, so the guard still
    # catches an accidentally-empty schedule on the walk path. It cost a whole
    # paired capture to learn -- the oracle exited with code 5 before rendering
    # a frame, and the run looked exactly like an emulator too slow to reach
    # gameplay.
    ORACLE_NO_INPUT=true
else
    ORACLE_NO_INPUT=false
    OURS_SCRIPT=$(gears_walk_ours)
    THEIRS_INPUT=$(gears_walk_theirs)
    if [ -z "$OURS_SCRIPT" ] || [ -z "$THEIRS_INPUT" ]; then
        echo "REFUSING: the walk generator produced an empty schedule, so neither" >&2
        echo "side would be driven and both would sit on the title screen." >&2
        exit 2
    fi
fi
for f in "$ORACLE" "$RUNTIME"; do
    [ -x "$f" ] || { echo "REFUSING: $f is not built. Nothing was run." >&2; exit 2; }
done
[ -f "$GAME_DIR/default.xex" ] || {
    echo "REFUSING: $GAME_DIR/default.xex is missing. Nothing was run." >&2; exit 2; }
# THE EXTRACTED TREE, not the disc image, unless GEARS_LAYER_ISO=1 asks for it.
# Two runs off the .iso stalled mid-boot -- the guest stopped presenting at swap
# 123 and again at 553, with every emulator thread idle in a futex wait -- and
# the same build off the local extracted tree walked to gameplay every time. The
# image lives on a slow mount here; the tree is what our runtime reads anyway,
# so this also removes one difference between the two arms.
if [ "${GEARS_LAYER_ISO:-0}" = "1" ] && [ -n "${GEARS_ISO:-}" ] && \
   [ -f "${GEARS_ISO:-}" ]; then
    ORACLE_TARGET="$GEARS_ISO"
else
    ORACLE_TARGET="$GAME_DIR/default.xex"
fi

rm -rf "$OUT"; mkdir -p "$OUT/ours" "$OUT/theirs"
echo "selector: $GEARS_LAYER_AFTER frame(s) after the first with"
echo "          >= $GEARS_LAYER_MIN_DRAWS draws,"
echo "          applied identically to both sides; $SECONDS_TO_RUN s per side"

# Both sides run in the background and are killed BY PID. TERM first, with a
# grace period: a SIGKILL landing mid-vkQueueSubmit can wedge the GPU and the
# next run then dies with VK_ERROR_DEVICE_LOST for reasons of its own making.
stop() {
    kill -TERM "$1" 2>/dev/null || true
    g=0; while [ "$g" -lt 20 ] && kill -0 "$1" 2>/dev/null; do sleep 1; g=$((g + 1)); done
    kill -9 "$1" 2>/dev/null || true
    wait "$1" 2>/dev/null || true
}

echo "== our renderer =="
GEARS_NO_WINDOW=1 \
GEARS_INPUT_SCRIPT="$OURS_SCRIPT" \
GEARS_DRAW_FRAME_MIN_DRAWS="$GEARS_LAYER_MIN_DRAWS" \
GEARS_DRAW_FRAME_AFTER_GAMEPLAY="$GEARS_LAYER_AFTER" \
GEARS_DRAW_FRAME_COUNT=1 \
GEARS_DRAW_RESOLVE_DUMP_EACH=1 \
GEARS_DRAW_DIR="$OUT/ours" \
    "$RUNTIME" "$GAME_DIR/default.xex" "$GAME_DIR" > "$OUT/ours.log" 2>&1 &
ours_pid=$!
trap 'kill -9 "$ours_pid" 2>/dev/null || true' EXIT INT TERM
waited=0
while [ "$waited" -lt "$SECONDS_TO_RUN" ]; do
    kill -0 "$ours_pid" 2>/dev/null || break
    # Stop as soon as the capture exists -- the run has nothing left to do.
    [ -n "$(find "$OUT/ours" -name 'resolve_*.ppm' 2>/dev/null | head -1)" ] && break
    sleep 5; waited=$((waited + 5))
done
stop "$ours_pid"; trap - EXIT INT TERM

echo "== the oracle =="
SDL_AUDIODRIVER=dummy \
GEARS_ORACLE_RESOLVE_DUMP="$OUT/theirs" \
GEARS_ORACLE_DUMP_MIN_DRAWS="$GEARS_LAYER_MIN_DRAWS" \
GEARS_ORACLE_DUMP_AFTER_GAMEPLAY="$GEARS_LAYER_AFTER" \
    "$ORACLE" \
    --store_shaders=false \
    --target="$ORACLE_TARGET" \
    --oracle_out="$OUT/theirs_frames" \
    --oracle_by_frame=true \
    --oracle_frames=$((SECONDS_TO_RUN * 30)) \
    --oracle_frame_interval=1200 \
    --oracle_allow_no_input=$ORACLE_NO_INPUT \
    --oracle_input="$THEIRS_INPUT" > "$OUT/theirs.log" 2>&1 &
theirs_pid=$!
trap 'kill -9 "$theirs_pid" 2>/dev/null || true' EXIT INT TERM
waited=0
while [ "$waited" -lt "$SECONDS_TO_RUN" ]; do
    kill -0 "$theirs_pid" 2>/dev/null || break
    [ -n "$(find "$OUT/theirs" -name 'oracle_f*.bin' 2>/dev/null | head -1)" ] && {
        # The dump covers one frame; give it time to finish that frame's copies.
        sleep 20; break; }
    sleep 5; waited=$((waited + 5))
done
stop "$theirs_pid"; trap - EXIT INT TERM

# WHICH FRAME EACH SIDE ACTUALLY SELECTED, always printed. A capture whose two
# halves came from different moments is the failure this script exists to
# prevent, and it is invisible in the images themselves.
echo
echo "== what each side selected =="
grep -h "is the capture\|frames scanned, none\|NOTHING has been" "$OUT/ours.log" | tail -3
grep -h "so it is gameplay\|none with >=" "$OUT/theirs.log" | tail -3
# WHY THE ORACLE PRODUCED NOTHING, when it produced nothing. Its own refusals
# are the first thing to read and they are 900 lines into a boot log, so a run
# that dumps zero passes reads as "the emulator was too slow" -- which is what a
# refusal at startup (exit code 5, no frame rendered) looked like for a whole
# capture. Printed only in the failing case, so a good run stays quiet.
if [ -z "$(find "$OUT/theirs" -name 'oracle_f*.bin' 2>/dev/null | head -1)" ]; then
    echo "the oracle dumped NOTHING. Its own errors, if any:"
    grep -h "oracle:" "$OUT/theirs.log" | grep -i "refus\|STOPPED\|failed" | tail -5
fi
echo
exec python3 "$REPO/tools/layer_compare.py" --ours "$OUT/ours" --theirs "$OUT/theirs" \
     --out "$OUT/layers"

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

# ...AND THE CONSOLE'S PER-FRAME DRAW STREAM, for the same reason. It counts
# draws per (vertex shader, pixel shader) pair with the SAME FNV hash of the
# guest microcode our diag table carries, so "does the console issue the same
# draws for this pass" is answerable from a paired capture instead of by eye.
# That is the open half of catalog #91: everything the shadow-mask pass READS
# now matches the console, so the question is what it DRAWS.

# THE PER-DRAW TABLE IS TAKEN IN THE SAME RUN AS THE DUMPS. Without it a pass
# that differs can only be chased in a SEPARATE run, and this frame's passes
# vary run to run -- one shadow-atlas tile is empty in one run and takes 68,622
# fragments in the next (catalog #91). Attributing a pass difference to a draw
# from a different run is attributing it to a different frame.
echo "== our renderer =="
GEARS_NO_WINDOW=1 \
GEARS_INPUT_SCRIPT="$OURS_SCRIPT" \
GEARS_DRAW_FRAME_MIN_DRAWS="$GEARS_LAYER_MIN_DRAWS" \
GEARS_DRAW_FRAME_AFTER_GAMEPLAY="$GEARS_LAYER_AFTER" \
GEARS_DRAW_FRAME_COUNT=1 \
GEARS_DRAW_RESOLVE_DUMP_EACH=1 \
GEARS_DRAW_DIAG="$OUT/ours/draws.tsv" \
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

# THE CONSOLE DUMPS A WINDOW OF FRAMES, NOT ONE. Both emulators advance the
# guest by WALL-CLOCK delta time, so the same frame INDEX is not the same game
# time: two runs of this oracle dumped their frame 875 and their frame 873 with
# a DIFFERENT NUMBER OF SHADOW-CASTING LIGHTS, and every per-pass number in the
# second was read as a renderer difference when the two sides were not looking
# at the same scene. layer_compare picks the frame whose PASS STRUCTURE matches
# ours, prints what each candidate scored, and says so loudly when none does.
# TWELVE, not five: OUR side drifts too, and a five-frame window twice failed
# to contain a frame with our pass structure -- the number of shadow-casting
# lights differs between neighbouring frames. Each dumped frame costs about
# 57 MB of scratch and a second of the oracle's run.
: "${GEARS_LAYER_ORACLE_FRAMES:=12}"

# THE ORACLE IS GIVEN THE SCRIPT'S OWN PATIENCE, not its built-in default.
# Its per-target wait defaults to 240 s while this script waits 420, so a boot
# that is merely SLOW was ending the oracle at guest frame 546 with three
# minutes of budget left -- and the run then reported "the title stopped
# presenting", which is a stall, when nothing had stalled. One number, passed
# in, so the two cannot disagree again.

# THE ORACLE'S BOOT IS FLAKY, and a paired capture costs seven minutes. Twice
# in four runs it stopped presenting at guest frame 1 -- the title never got
# going -- and the run then produced nothing and had to be noticed and started
# again by hand. That is retried here, a bounded number of times, and ONLY on
# that signature: a run that reached gameplay and dumped nothing for some other
# reason must still fail loudly. Every attempt is counted and the count is
# printed, so a capture that needed three tries never looks like one that
# worked first time.
: "${GEARS_LAYER_ORACLE_TRIES:=3}"
oracle_try=0
while : ; do
    oracle_try=$((oracle_try + 1))
echo "== the oracle (attempt $oracle_try of $GEARS_LAYER_ORACLE_TRIES) =="
SDL_AUDIODRIVER=dummy \
GEARS_ORACLE_RESOLVE_DUMP="$OUT/theirs" \
GEARS_ORACLE_DUMP_MIN_DRAWS="$GEARS_LAYER_MIN_DRAWS" \
GEARS_ORACLE_DUMP_AFTER_GAMEPLAY="$GEARS_LAYER_AFTER" \
GEARS_ORACLE_DUMP_FRAMES="$GEARS_LAYER_ORACLE_FRAMES" \
GEARS_ORACLE_DRAW_STREAM="$OUT/theirs_draws.tsv" \
    "$ORACLE" \
    --store_shaders=false \
    --target="$ORACLE_TARGET" \
    --oracle_out="$OUT/theirs_frames" \
    --oracle_by_frame=true \
    --oracle_frames=$((SECONDS_TO_RUN * 30)) \
    --oracle_frame_interval=1200 \
    --oracle_frame_timeout="$SECONDS_TO_RUN" \
    --oracle_allow_no_input=$ORACLE_NO_INPUT \
    --oracle_input="$THEIRS_INPUT" > "$OUT/theirs.log" 2>&1 &
theirs_pid=$!
trap 'kill -9 "$theirs_pid" 2>/dev/null || true' EXIT INT TERM
waited=0
while [ "$waited" -lt "$SECONDS_TO_RUN" ]; do
    kill -0 "$theirs_pid" 2>/dev/null || break
    [ -n "$(find "$OUT/theirs" -name 'oracle_f*.bin' 2>/dev/null | head -1)" ] && {
        # The dump covers a WINDOW of frames; give it time to finish them.
        # It runs at roughly one dumped frame per second, and stopping the
        # oracle early truncates the window into a partial last frame that
        # would then look like a console that resolved fewer passes.
        sleep $((10 + GEARS_LAYER_ORACLE_FRAMES * 10)); break; }
    sleep 5; waited=$((waited + 5))
done
stop "$theirs_pid"; trap - EXIT INT TERM
    if [ -n "$(find "$OUT/theirs" -name 'oracle_f*.bin' 2>/dev/null | head -1)" ]; then
        break
    fi
    if [ "$oracle_try" -ge "$GEARS_LAYER_ORACLE_TRIES" ]; then
        echo "the oracle produced nothing in $oracle_try attempt(s); giving up"
        break
    fi
    # A DEVICE LOSS IS NEVER RETRIED. The kernel is resetting the card that is
    # also drawing the desktop, and a retry loop into that is how a lost
    # measurement becomes a lost session. This gate comes FIRST because a lost
    # device also stops the title presenting, so the "boot did not take" gate
    # below would otherwise match it and retry -- the oracle really did lose
    # the device once here (radv "the CS has been cancelled because the context
    # is lost"), and the run after it looked like an ordinary flaky boot.
    if grep -qi "DEVICE_LOST\|Graphics device lost" "$OUT/theirs.log" 2>/dev/null; then
        echo "REFUSING TO RETRY: the oracle LOST THE VULKAN DEVICE. That is a" >&2
        echo "GPU fault, not a flaky boot, and retrying submits more work to a" >&2
        echo "card the kernel is resetting. Nothing further was run. The lines:" >&2
        grep -i "DEVICE_LOST\|Graphics device lost\|context is lost" \
            "$OUT/theirs.log" 2>/dev/null | head -5 >&2
        exit 3
    fi
    # A BOOT THAT DID NOT TAKE IS A STALL BEFORE GAMEPLAY, at whatever frame.
    # The gate used to be the exact string "guest frame 1", and a run then
    # wedged at frame 123 -- the process alive, its log untouched for five
    # minutes, the same worthless capture -- and was not retried because the
    # number was different. What distinguishes "the boot did not take" from a
    # real finding is not WHICH frame it died at: it is that the title stopped
    # presenting before it ever reached gameplay, which the oracle says in the
    # same line, and that it dumped nothing.
    if grep -q "STOPPED at guest frame .* waiting" "$OUT/theirs.log" 2>/dev/null &&
       ! grep -q "so it is gameplay" "$OUT/theirs.log" 2>/dev/null; then
        stalled_at=$(grep -o "STOPPED at guest frame [0-9]*" "$OUT/theirs.log" |
                     tail -1 | grep -o "[0-9]*$")
        echo "the oracle stopped presenting at guest frame ${stalled_at:-?}, before"
        echo "reaching gameplay -- a boot that did not take. Retrying"
        echo "(attempt $((oracle_try + 1)))."
        mv -f "$OUT/theirs.log" "$OUT/theirs.attempt$oracle_try.log" 2>/dev/null || true
        continue
    fi
    echo "the oracle dumped nothing, and it is NOT the flaky boot: it either"
    echo "reached gameplay or failed some other way. Not retrying -- read"
    echo "theirs.log."
    break
done
echo "oracle attempts: $oracle_try"

# WHICH FRAME EACH SIDE ACTUALLY SELECTED, always printed. A capture whose two
# halves came from different moments is the failure this script exists to
# prevent, and it is invisible in the images themselves.
echo
echo "== what each side selected =="
grep -h "is the capture\|frames scanned, none\|NOTHING has been" "$OUT/ours.log" | tail -3
# EVERY attempt's log, not just the last: a retried boot moves the failed one
# aside, and a report that read only the survivor would say nothing about the
# attempts that did not take.
grep -h "so it is gameplay\|none with >=\|STOPPED at guest frame" \
    "$OUT"/theirs*.log 2>/dev/null | tail -4
# WHY THE ORACLE PRODUCED NOTHING, when it produced nothing. Its own refusals
# are the first thing to read and they are 900 lines into a boot log, so a run
# that dumps zero passes reads as "the emulator was too slow" -- which is what a
# refusal at startup (exit code 5, no frame rendered) looked like for a whole
# capture. Printed only in the failing case, so a good run stays quiet.
if [ -z "$(find "$OUT/theirs" -name 'oracle_f*.bin' 2>/dev/null | head -1)" ]; then
    echo "the oracle dumped NOTHING. Its own errors, if any:"
    grep -h "oracle:" "$OUT"/theirs*.log 2>/dev/null |
        grep -i "refus\|STOPPED\|failed" | tail -5
fi
# THE TWO SIDES MUST HAVE CAPTURED THE SAME GUEST FRAME. The selector is
# content-based and applied identically, but it is applied to two runs: ours
# reached gameplay at guest frame 573 in one run and 482 in the next, while the
# oracle's boot is steadier. A pair taken 91 frames apart is two different game
# moments, and every row of the comparison would report that difference as the
# renderer's -- which is catalog #89, and it is invisible in the images.
ours_frame=$(sed -n 's/.*guest-draw: frame \([0-9]*\) is the capture.*/\1/p' \
             "$OUT/ours.log" | tail -1)
theirs_frame=$(sed -n 's/.*dumping every resolve of frame \([0-9]*\).*/\1/p' \
               "$OUT"/theirs*.log 2>/dev/null | tail -1)
# A SMALL GAP IS EXPECTED AND IS REPORTED ANYWAY. Both sides compute "the
# first frame with >= 400 draws, plus 300", and they cross that threshold a
# frame or two apart -- measured at 573 vs 574 on one run and 573 vs 573 on
# another. The tolerance is what separates that from the failure this guards
# against, which was 91 frames. It is printed on EVERY run, matching or not,
# because a two-frame gap is still two frames of animation and any row's
# difference has to be weighed against it.
: "${GEARS_LAYER_FRAME_TOLERANCE:=4}"
gap=""
if [ -n "$ours_frame" ] && [ -n "$theirs_frame" ]; then
    gap=$((ours_frame - theirs_frame))
    [ "$gap" -lt 0 ] && gap=$((-gap))
fi
echo "frame selected: ours ${ours_frame:-NONE}, theirs ${theirs_frame:-NONE}${gap:+ (gap $gap frame(s), tolerance $GEARS_LAYER_FRAME_TOLERANCE)}"
if [ -n "$gap" ] && [ "$gap" -gt "$GEARS_LAYER_FRAME_TOLERANCE" ]; then
    echo "REFUSING to compare: the two sides captured guest frames $ours_frame" >&2
    echo "and $theirs_frame, $gap apart. That is two game moments, and every row" >&2
    echo "would report the difference between them as the renderer's. Raise" >&2
    echo "GEARS_LAYER_FRAME_TOLERANCE only if you know the scene is static." >&2
    exit 3
fi
echo
exec python3 "$REPO/tools/layer_compare.py" --ours "$OUT/ours" --theirs "$OUT/theirs" \
     --out "$OUT/layers"

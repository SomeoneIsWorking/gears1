#!/bin/sh
# ONE oracle run, then OUR side camera-gated to THAT run -- the only pairing
# that has ever been shown to work, produced in a single command so the two
# halves cannot drift apart.
#
# WHY THIS EXISTS. Two separate scripts produced the two halves before, and
# nothing tied them together:
#
#   * claim C042 -- a capture from 11:34 was compared against oracle dumps from an
#     11:54 run, and that later run had OVERWRITTEN the very camera file the
#     capture was gated to. Two wrong conclusions were published before the file
#     timestamps gave it away.
#   * claim C043 -- the CONTENT selector that tools/layer_capture.sh applies to
#     both sides pairs them to a log-luminance correlation of only 0.49, against
#     0.94 for a genuine match. Content selection is not close enough for a
#     pixelwise comparison, however honestly it is applied.
#
# So: the oracle runs FIRST and dumps its resolves, its frame window and the
# vertex constants of a named SHADER. That constants file is FROZEN into
# the output (provenance.py --camera copies it in, rather than referencing a
# path a later run can overwrite). Our side then runs gated on the frozen copy,
# holding frames until the guest's own view-projection matches. Both halves are
# stamped with ONE pair id before either runs.
#
#   tools/camera_pair.sh [seconds-per-side] [out-dir] [vs-hash]
#   PRIM_STATS=<vs hash>   also measure the CONSOLE's per-draw pipeline
#                          statistics for that shader, on the SAME run that
#                          produces the camera -- so the counts belong to the
#                          frame our capture is gated to. Measuring them in a
#                          separate oracle run gives a different game moment,
#                          which supports a comparison of DISTRIBUTIONS and not
#                          a draw-to-draw attribution (catalog #91).
#   VS_CONSTS_ALL=<vs hash> retain every dumped-frame bind of another vertex
#                          shader on BOTH sides. Use this for pass matrices
#                          whose winning oracle frame is known only after the
#                          candidate window is inspected.
#   VDUMP_VS=<vs hash>:<min>-<max> fingerprint the complete guest index and
#                          vertex-fetch buffers of every matching draw on both
#                          sides. This compares geometry inputs, not aggregate
#                          primitive counts.
#
# THE CAMERA IS NAMED BY SHADER HASH, NOT BY DRAW ORDINAL. An ordinal is not
# stable across runs -- ordinal 294 was the camera shader in one run and a
# 12-vertex draw in the next, because the per-frame draw count varies with the
# number of shadow-casting lights. A run keyed on the ordinal silently dumps the
# constants of whatever draw happens to land there, and the gate then refuses
# when the selected four view-projection rows are absent.
#
# THE RESULT IS STRUCTURALLY PAIRED, NOT REDUCED TO AN AGGREGATE SCORE: the
# final comparison identifies the first differing resolved pass. Exact draw and
# ownership ledgers then locate the earliest divergence inside that pass.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/tools/env.sh"
GAME_DIR="${GEARS_GAME_DIR:-$REPO/scratch/game}"
# The full gameplay walk ends at f6150 and is unusably slow under oracle
# diagnostics. This pair-specific route is the measured minimum that reconciles
# the two boot states: START opens/pauses their current screen and A closes both
# paths. Native's automatic storage selector can close its title-owned modal
# because xam_notify.cpp now supplies the console's system-UI notifications.
#
# START MUST PRECEDE THE ORACLE'S STATIC TITLE FRAME. A fresh oracle boot stops
# presenting at guest frame 123 while waiting for input; a first event at f450
# is therefore unreachable because frame-driven input and the frame counter
# wait on each other forever. The old 450/600 route failed with 123 frames,
# 1 draw/frame and 0/8 qualified captures. The 110/260 route fired both arms
# and reached a 1156-draw gameplay frame at f900. A at f260 was too early for
# native's storage dialog and left it over the capture; f600 reaches the dialog,
# invokes the automatic selector and produces a clean frame. Camera-pair
# evidence is in C055 / issues #102-103. Do not move the first event later
# without repeating that negative/positive discriminator on the shipping oracle.
: "${GEARS_CAMERA_PAIR_WALK_TABLE:=90:START~120 600:A}"
: "${GEARS_WALK_TABLE:=$GEARS_CAMERA_PAIR_WALK_TABLE}"
export GEARS_WALK_TABLE
. "$REPO/tools/menu_walk.sh"
# A paired run must drive both guests from ONE semantic input table.  The two
# runtimes use different syntax, so menu_walk.sh generates both strings.  A
# caller may override GEARS_WALK_TABLE, but overriding only GEARS_INPUT_SCRIPT
# would put the two guests back into different UI states and is deliberately
# ignored here.
STARTUP_MAP=$(python3 "$REPO/tools/startup_map.py" show --game-dir "$GAME_DIR" \
    2>/dev/null | sed -n 's/.*LocalMap=\([^ ]*\).*/\1/p' | head -1)
case "${STARTUP_MAP:-}" in
    ""|[Ww]ar[Ss]tart*)
        DIRECT_BOOT=0
        PAIR_INPUT_OURS=$(gears_walk_ours)
        PAIR_INPUT_THEIRS=$(gears_walk_theirs)
        WALK_LAST_FRAME=$(gears_walk_last_frame)
        ;;
    *)
        DIRECT_BOOT=1
        PAIR_INPUT_OURS=""
        PAIR_INPUT_THEIRS=""
        WALK_LAST_FRAME=0
        echo "startup map is '$STARTUP_MAP': both guests boot it directly with no pad schedule"
        ;;
esac
GEARS_INPUT_SCRIPT=$PAIR_INPUT_OURS
export GEARS_INPUT_SCRIPT

SECONDS_TO_RUN="${1:-300}"
OUT="${2:-$REPO/scratch/camerapair}"
VS_HASH="${3:-f3e9368c1bb68ecc}"
ORACLE="$REPO/scratch/oracle/oracle-build/xenia_oracle"
RUNTIME="${GEARS_BUILD_DIR:-$REPO/scratch/build}/runtime/gears1"
: "${GEARS_LAYER_MIN_DRAWS:=400}"
: "${GEARS_LAYER_AFTER:=300}"
: "${CAMERA_NEAR:=0.013}"
: "${CAMERA_ROT_NEAR:=0.005}"
: "${CAMERA_CONST_BASE:=230}"
case "$CAMERA_CONST_BASE" in
    ''|*[!0-9]*) echo "REFUSING: CAMERA_CONST_BASE must be a decimal constant index" >&2; exit 2 ;;
esac
[ "$CAMERA_CONST_BASE" -le 252 ] || {
    echo "REFUSING: CAMERA_CONST_BASE=$CAMERA_CONST_BASE cannot name four rows in c0..c255" >&2
    exit 2
}
[ -x "$ORACLE" ]  || { echo "REFUSING: $ORACLE not built" >&2; exit 2; }
[ -x "$RUNTIME" ] || { echo "REFUSING: $RUNTIME not built" >&2; exit 2; }
[ -f "$GAME_DIR/default.xex" ] || { echo "REFUSING: no default.xex" >&2; exit 2; }

"$REPO/tools/cleanup_scratch_path.sh" "$OUT"
mkdir -p "$OUT/ours" "$OUT/theirs"
PAIR="camerapair-$(date -u +%Y%m%dT%H%M%SZ)-$$"
CONSTS="$REPO/scratch/oracle/vs_consts.txt"
"$REPO/tools/cleanup_scratch_path.sh" "$CONSTS"

# ---------------------------------------------------------------- the console
echo "== the console, $SECONDS_TO_RUN s: resolves, a frame window, and the"
echo "   vertex constants of shader $VS_HASH =="
cd "$REPO"
SDL_AUDIODRIVER=dummy \
GEARS_ORACLE_RESOLVE_DUMP="$OUT/theirs" \
GEARS_ORACLE_DUMP_MIN_DRAWS="$GEARS_LAYER_MIN_DRAWS" \
GEARS_ORACLE_DUMP_MIN_GUEST_FRAME="$WALK_LAST_FRAME" \
GEARS_ORACLE_DUMP_AFTER_GAMEPLAY="$GEARS_LAYER_AFTER" \
GEARS_ORACLE_DUMP_FRAMES="${GEARS_ORACLE_DUMP_FRAMES:-5}" \
GEARS_ORACLE_DRAW_ORDER="$OUT/theirs_order.tsv" \
GEARS_ORACLE_VS_CONSTS="$VS_HASH" \
GEARS_ORACLE_VS_CONSTS_ALL="${VS_CONSTS_ALL:-}" \
GEARS_ORACLE_VS_CONSTS_ALL_OUT="$OUT/theirs_vs_consts_all.txt" \
GEARS_ORACLE_VDUMP_VS="${VDUMP_VS:-}" \
GEARS_ORACLE_VDUMP_VS_OUT="$OUT/theirs_geometry.txt" \
GEARS_ORACLE_PRIM_STATS="${PRIM_STATS:-}" \
    "$ORACLE" \
    --store_shaders=false \
    --target="$GAME_DIR/default.xex" \
    --oracle_out="$OUT/theirs_frames" \
    --oracle_by_frame=true \
    --oracle_frames=$((SECONDS_TO_RUN * 30)) \
    --oracle_frame_interval=1200 \
    --oracle_frame_timeout="$SECONDS_TO_RUN" \
    --oracle_allow_no_input=$([ "$DIRECT_BOOT" = 1 ] && echo true || echo false) \
    --oracle_input="$PAIR_INPUT_THEIRS" > "$OUT/theirs.log" 2>&1 &
opid=$!
trap 'kill -9 "$opid" 2>/dev/null || true' EXIT INT TERM
# WAIT FOR THE WHOLE WINDOW, NOT FOR THE CONSTANTS. The constants land on the
# FIRST dumped frame, so breaking on them kills the console at the very start of
# its window: asking for 45 frames and getting 4 or 5 every time. That defect
# made a wider window impossible and looked like the window parameter being
# ignored (catalog #87). The wait now counts the front-buffer dumps -- one per
# dumped frame -- and reports progress, so a run that stalls says how far it got.
WANT="${GEARS_ORACLE_DUMP_FRAMES:-5}"
w=0; got=0
while [ "$w" -lt "$SECONDS_TO_RUN" ]; do
    kill -0 "$opid" 2>/dev/null || break
    got=$(ls "$OUT/theirs" 2>/dev/null | grep -c "_f6_e0_" || true)
    [ "$got" -ge "$WANT" ] && { echo "   console dumped $got/$WANT frames"; break; }
    [ $((w % 60)) -eq 0 ] && [ "$w" -gt 0 ] && \
        echo "   ... ${w}s: $got/$WANT console frames dumped"
    sleep 5; w=$((w + 5))
done
[ "$got" -ge "$WANT" ] || echo "   console reached only $got of $WANT frames in ${w}s; structural pairing will use what exists, but the shortened window may omit the intended camera moment"
kill -TERM "$opid" 2>/dev/null || true
g=0; while [ "$g" -lt 20 ] && kill -0 "$opid" 2>/dev/null; do sleep 1; g=$((g + 1)); done
kill -9 "$opid" 2>/dev/null || true
wait "$opid" 2>/dev/null || true
trap - EXIT INT TERM

# Prove the oracle accepted the generated half of the shared walk.  The old
# harness explicitly selected no input here while native used a timed walk;
# matching the 3D camera behind different menus then passed the pixel gate.
oracle_input_lines=$(grep -c 'oracle:.*input' "$OUT/theirs.log" 2>/dev/null || true)
oracle_schedules=$(grep -c 'oracle: [0-9][0-9]* scheduled press(es)' \
    "$OUT/theirs.log" 2>/dev/null || true)
oracle_frame_driven=$(grep -c 'oracle: input and captures are driven by the GUEST FRAME COUNTER' \
    "$OUT/theirs.log" 2>/dev/null || true)
oracle_no_input=$(grep -c 'oracle: NO input schedule, by request' \
    "$OUT/theirs.log" 2>/dev/null || true)
if [ "$DIRECT_BOOT" = 1 ]; then
    if [ "$oracle_no_input" -ne 1 ] || [ "$oracle_schedules" -ne 0 ]; then
        echo "REFUSING: direct-boot oracle input validation scanned $oracle_input_lines line(s)," >&2
        echo "found $oracle_no_input no-input declaration(s) and $oracle_schedules schedule(s)." >&2
        exit 10
    fi
    echo "   oracle input validated: direct startup-map boot, one explicit no-input declaration"
elif [ "$oracle_schedules" -ne 1 ] || [ "$oracle_frame_driven" -ne 1 ]; then
    echo "REFUSING: oracle input validation scanned $oracle_input_lines input log line(s)," >&2
    echo "found $oracle_schedules schedule declaration(s) and $oracle_frame_driven" >&2
    echo "guest-frame declaration(s). Both guests must accept the shared walk." >&2
    grep 'oracle:.*input' "$OUT/theirs.log" | head -5 >&2 || true
    exit 10
else
    echo "   oracle input validated: scanned $oracle_input_lines input log line(s), one shared schedule"
fi

# A GPU fault ends the run. It is never retried here, and it is never treated
# as an environmental hiccup: the FIRST device loss stops the session's GPU work.
if grep -qi "DEVICE_LOST\|Graphics device lost\|context is lost" "$OUT/theirs.log"; then
    echo "THE ORACLE LOST THE VULKAN DEVICE. Nothing is retried." >&2
    grep -i "DEVICE_LOST\|Graphics device lost\|context is lost" "$OUT/theirs.log" | head -5 >&2
    exit 3
fi
[ -s "$CONSTS" ] || {
    echo "REFUSING: the console never dumped constants for shader $VS_HASH" >&2
    echo "in $SECONDS_TO_RUN s. Either it did not reach the dump window, or" >&2
    echo "that shader never bound inside it. This run measured NOTHING -- it" >&2
    echo "is not an empty result. \$OUT/theirs_order.tsv lists the shaders the" >&2
    echo "dumped frame actually drew." >&2
    exit 4; }

# THE CAMERA MUST COME FROM A FRAME THE CONSOLE DUMPED. Otherwise it names a
# viewpoint there are no console resolves for, and the pair cannot be compared --
# which is the failure that cost two runs here.
CFRAME=$(sed -n 's/.*at guest frame \([0-9]*\).*/\1/p' "$CONSTS" | head -1)
if [ -n "$CFRAME" ] && ! ls "$OUT/theirs" | grep -q "_f${CFRAME}_"; then
    echo "REFUSING: the camera constants are from guest frame $CFRAME, and the" >&2
    echo "console dumped no resolves for that frame. The gate would hold our" >&2
    echo "side at a viewpoint there is nothing to compare against. Dumped" >&2
    echo "frames: $(ls "$OUT/theirs" | sed -n 's/.*_f\([0-9]*\)_copy.*/\1/p' | sort -un | tr '\n' ' ')" >&2
    exit 8
fi
camera_rows=0
camera_end=$((CAMERA_CONST_BASE + 3))
camera_idx=$CAMERA_CONST_BASE
while [ "$camera_idx" -le "$camera_end" ]; do
    grep -q "^c\[$camera_idx\]" "$CONSTS" && camera_rows=$((camera_rows + 1))
    camera_idx=$((camera_idx + 1))
done
[ "$camera_rows" -eq 4 ] || {
    echo "REFUSING: $CONSTS carries $camera_rows of c$CAMERA_CONST_BASE..c$camera_end," >&2
    echo "so the selected shader/constant layout does not provide a camera. The" >&2
    echo "gate would run with no viewpoint at all. First line: $(head -1 "$CONSTS")" >&2
    exit 9; }
echo "   camera: shader $VS_HASH at guest frame $CFRAME, inside the dumped window"
echo "   console dumped $(ls "$OUT/theirs" | wc -l) file(s); constants captured"

# The camera is FROZEN into both directories here, before our side runs, so a
# later oracle run cannot substitute it underneath the comparison.
python3 "$REPO/tools/provenance.py" stamp "$OUT/theirs" --role theirs --pair "$PAIR" \
    --camera "$CONSTS" --note "vs=$VS_HASH" --note "script=camera_pair.sh"
python3 "$REPO/tools/provenance.py" stamp "$OUT/ours" --role ours --pair "$PAIR" \
    --camera "$CONSTS" --note "vs=$VS_HASH" --note "script=camera_pair.sh" \
    --note "camera_near=$CAMERA_NEAR" --note "camera_rot_near=$CAMERA_ROT_NEAR" \
    --note "camera_const_base=$CAMERA_CONST_BASE"
FROZEN="$OUT/ours/camera.txt"

# -------------------------------------------------------------------- our side
echo "== our renderer, $SECONDS_TO_RUN s, gated on the console's own viewpoint =="
GEARS_NO_WINDOW=1 \
GEARS_DRAW_FRAME_MIN_DRAWS="$GEARS_LAYER_MIN_DRAWS" \
GEARS_DRAW_FRAME_MIN_GUEST_FRAME="$WALK_LAST_FRAME" \
GEARS_DRAW_FRAME_AFTER_GAMEPLAY="$GEARS_LAYER_AFTER" \
GEARS_DRAW_FRAME_COUNT=1 \
GEARS_DRAW_FRAME_NEEDS="$VS_HASH" \
GEARS_DRAW_FRAME_CAMERA="$FROZEN:$CAMERA_NEAR:$CAMERA_ROT_NEAR:$CAMERA_CONST_BASE" \
GEARS_DRAW_VS_CONSTS_VS="${VS_CONSTS_ALL:-}" \
GEARS_DRAW_VDUMP_VS="${VDUMP_VS:-}" \
GEARS_DRAW_RESOLVE_DUMP_EACH=1 \
GEARS_DRAW_DIAG="$OUT/ours/draws.tsv" \
GEARS_DRAW_DIR="$OUT/ours" \
GEARS_INPUT_SCRIPT="$GEARS_INPUT_SCRIPT" \
    "$RUNTIME" "$GAME_DIR/default.xex" "$GAME_DIR" > "$OUT/ours.log" 2>&1 &
rpid=$!
trap 'kill -9 "$rpid" 2>/dev/null || true' EXIT INT TERM
w=0
while [ "$w" -lt "$SECONDS_TO_RUN" ]; do
    kill -0 "$rpid" 2>/dev/null || break
    grep -q "frame screenshot written" "$OUT/ours.log" 2>/dev/null && break
    sleep 5; w=$((w + 5))
done
kill -TERM "$rpid" 2>/dev/null || true
g=0; while [ "$g" -lt 20 ] && kill -0 "$rpid" 2>/dev/null; do sleep 1; g=$((g + 1)); done
kill -9 "$rpid" 2>/dev/null || true
wait "$rpid" 2>/dev/null || true
trap - EXIT INT TERM

# A close camera is not proof that both guests reached the same UI state.  A
# disconnected native pad once left the NO STORAGE DEVICE modal over gameplay;
# a whole-frame correlation hid it because most background pixels still agreed.
# Prove both that the runtime accepted the script and that the title polled far
# enough for at least one scripted transition to fire.
#
# The patterns are NOT line-anchored: lucent prefixes every log line with a
# timestamp, so the marker sits mid-line, and a ^-anchored grep validates
# nothing while reading as one (the ch45 pair of 2026-08-25 captured cleanly
# and was then refused by its own stale check).
input_lines=$(grep -c '\[input\]' "$OUT/ours.log" 2>/dev/null || true)
scripted_sources=$(grep -c '\[input\] scripted input:' "$OUT/ours.log" 2>/dev/null || true)
scripted_steps=$(grep -c '\[input\] scripted pad at ' "$OUT/ours.log" 2>/dev/null || true)
if [ "$DIRECT_BOOT" = 1 ]; then
    if [ "$scripted_sources" -ne 0 ] || [ "$scripted_steps" -ne 0 ]; then
        echo "REFUSING: direct-boot native unexpectedly accepted/fired a pad schedule" >&2
        exit 10
    fi
    echo "   native input validated: direct startup-map boot, no scripted pad transitions"
elif [ "$scripted_sources" -ne 1 ] || [ "$scripted_steps" -lt 1 ]; then
    echo "REFUSING: native input validation scanned $input_lines input log line(s)," >&2
    echo "found $scripted_sources scripted-source declaration(s) and $scripted_steps" >&2
    echo "fired scripted step(s). The camera can match behind a modal, so this pair" >&2
    echo "cannot support a pixel or pass comparison." >&2
    grep '^\[input\]' "$OUT/ours.log" | head -5 >&2 || true
    exit 10
else
    echo "   native input validated: scanned $input_lines input log line(s), one scripted source, $scripted_steps fired step(s)"
fi

selector_calls=$(grep -c '\[xam\] storage device selected automatically:' \
    "$OUT/ours.log" 2>/dev/null || true)
expected_selectors=$([ "$DIRECT_BOOT" = 1 ] && echo 0 || echo 1)
if [ "$selector_calls" -ne "$expected_selectors" ]; then
    echo "REFUSING: native logged $selector_calls automatic storage selections;" >&2
    echo "$expected_selectors required for direct_boot=$DIRECT_BOOT before UI-state comparison." >&2
    exit 11
fi

# Input delivery is not UI-state proof. An A press at f260 fired on both arms
# but arrived before native's storage dialog existed; the captured frame later
# retained NO STORAGE DEVICE while the oracle did not. The modal is eight extra
# draws of a measured UI shader pair. Refuse before a moving-scene drift curve
# can price the overlay as ordinary temporal change (issue #103).
python3 "$REPO/tools/ui_state_check.py" "$OUT/ours/draws.tsv" \
    --oracle "$OUT/theirs_order.tsv" || exit 11

if grep -qi "DEVICE_LOST\|Graphics device lost" "$OUT/ours.log"; then
    echo "OUR RENDERER LOST THE VULKAN DEVICE. Nothing is retried." >&2
    exit 3
fi
grep -E "CAMERA MATCHED|held for the CAMERA|NO camera gate" "$OUT/ours.log" | tail -3
grep -q "CAMERA MATCHED" "$OUT/ours.log" || {
    echo "THE CAMERA GATE NEVER MATCHED in $SECONDS_TO_RUN s. Nothing was" >&2
    echo "captured; the lines above say how close it came and to what. This" >&2
    echo "run measured NOTHING." >&2
    exit 6; }

# ---------------------------------------------------------- structural passes
echo "== structurally pairing every resolved pass =="
python3 "$REPO/tools/provenance.py" check "$OUT/ours" "$OUT/theirs" || exit 7
exec python3 "$REPO/tools/layer_compare.py" \
    --ours "$OUT/ours" --theirs "$OUT/theirs" --out "$OUT/layers"

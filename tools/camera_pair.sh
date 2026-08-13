#!/bin/sh
# ONE oracle run, then OUR side camera-gated to THAT run -- the only pairing
# that has ever been shown to work, produced in a single command so the two
# halves cannot drift apart.
#
# WHY THIS EXISTS. Two separate scripts produced the two halves before, and
# nothing tied them together:
#
#   * claim C042 -- a capture from 11:34 was scored against oracle dumps from an
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
#
# THE CAMERA IS NAMED BY SHADER HASH, NOT BY DRAW ORDINAL. An ordinal is not
# stable across runs -- ordinal 294 was the camera shader in one run and a
# 12-vertex draw in the next, because the per-frame draw count varies with the
# number of shadow-casting lights. A run keyed on the ordinal silently dumps the
# constants of whatever draw happens to land there, and the gate then refuses
# with "0 of the 4 view-projection rows c230..c233".
#
# THE RESULT IS SCORED, NOT ASSUMED: it finishes by running the same-picture
# gate over every console candidate and printing each score against the positive
# control. A run whose best candidate is below the gate has produced a capture
# that CANNOT support a pixelwise comparison, and it says so and exits non-zero
# rather than leaving a directory that looks usable.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/tools/env.sh"
# The console can reach its capture window from its autonomous boot path, but
# the native runtime remains at the title screen when headless has no pad
# source.  A paired capture must therefore supply the same maintained walk the
# other gameplay capture tools use; depending on a caller's exported shell
# state made this script silently produce an oracle-only directory.
. "$REPO/tools/menu_walk.sh"
: "${GEARS_INPUT_SCRIPT:=$GEARS_MENU_WALK}"
export GEARS_INPUT_SCRIPT

SECONDS_TO_RUN="${1:-300}"
OUT="${2:-$REPO/scratch/camerapair}"
VS_HASH="${3:-f3e9368c1bb68ecc}"
GAME_DIR="${GEARS_GAME_DIR:-$REPO/scratch/game}"
ORACLE="$REPO/scratch/oracle/oracle-build/xenia_oracle"
RUNTIME="${GEARS_BUILD_DIR:-$REPO/scratch/build}/runtime/gears1"
: "${GEARS_LAYER_MIN_DRAWS:=400}"
: "${GEARS_LAYER_AFTER:=300}"
: "${CAMERA_NEAR:=10}"

[ -x "$ORACLE" ]  || { echo "REFUSING: $ORACLE not built" >&2; exit 2; }
[ -x "$RUNTIME" ] || { echo "REFUSING: $RUNTIME not built" >&2; exit 2; }
[ -f "$GAME_DIR/default.xex" ] || { echo "REFUSING: no default.xex" >&2; exit 2; }

rm -rf "$OUT"; mkdir -p "$OUT/ours" "$OUT/theirs"
PAIR="camerapair-$(date -u +%Y%m%dT%H%M%SZ)-$$"
CONSTS="$REPO/scratch/oracle/vs_consts.txt"
rm -f "$CONSTS"

# ---------------------------------------------------------------- the console
echo "== the console, $SECONDS_TO_RUN s: resolves, a frame window, and the"
echo "   vertex constants of shader $VS_HASH =="
cd "$REPO"
SDL_AUDIODRIVER=dummy \
GEARS_ORACLE_RESOLVE_DUMP="$OUT/theirs" \
GEARS_ORACLE_DUMP_MIN_DRAWS="$GEARS_LAYER_MIN_DRAWS" \
GEARS_ORACLE_DUMP_AFTER_GAMEPLAY="$GEARS_LAYER_AFTER" \
GEARS_ORACLE_DUMP_FRAMES="${GEARS_ORACLE_DUMP_FRAMES:-5}" \
GEARS_ORACLE_DRAW_ORDER="$OUT/theirs_order.tsv" \
GEARS_ORACLE_VS_CONSTS="$VS_HASH" \
GEARS_ORACLE_PRIM_STATS="${PRIM_STATS:-}" \
    "$ORACLE" \
    --store_shaders=false \
    --target="$GAME_DIR/default.xex" \
    --oracle_out="$OUT/theirs_frames" \
    --oracle_by_frame=true \
    --oracle_frames=$((SECONDS_TO_RUN * 30)) \
    --oracle_frame_interval=1200 \
    --oracle_frame_timeout="$SECONDS_TO_RUN" \
    --oracle_allow_no_input=true \
    --oracle_input="" > "$OUT/theirs.log" 2>&1 &
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
[ "$got" -ge "$WANT" ] || echo "   console reached only $got of $WANT frames in ${w}s; scoring will use what there is, and a BOUNDARY peak in the scores means the window still did not span our moment"
kill -TERM "$opid" 2>/dev/null || true
g=0; while [ "$g" -lt 20 ] && kill -0 "$opid" 2>/dev/null; do sleep 1; g=$((g + 1)); done
kill -9 "$opid" 2>/dev/null || true
wait "$opid" 2>/dev/null || true
trap - EXIT INT TERM

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
# viewpoint there are no console resolves for, and the pair cannot be scored --
# which is the failure that cost two runs here.
CFRAME=$(sed -n 's/.*at guest frame \([0-9]*\).*/\1/p' "$CONSTS" | head -1)
if [ -n "$CFRAME" ] && ! ls "$OUT/theirs" | grep -q "_f${CFRAME}_"; then
    echo "REFUSING: the camera constants are from guest frame $CFRAME, and the" >&2
    echo "console dumped no resolves for that frame. The gate would hold our" >&2
    echo "side at a viewpoint there is nothing to compare against. Dumped" >&2
    echo "frames: $(ls "$OUT/theirs" | sed -n 's/.*_f\([0-9]*\)_copy.*/\1/p' | sort -un | tr '\n' ' ')" >&2
    exit 8
fi
grep -q "^c\[23[0-3]\]" "$CONSTS" || {
    echo "REFUSING: $CONSTS carries no c230..c233, so it is not a camera. The" >&2
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
    --note "camera_near=$CAMERA_NEAR"
FROZEN="$OUT/ours/camera.txt"

# -------------------------------------------------------------------- our side
echo "== our renderer, $SECONDS_TO_RUN s, gated on the console's own viewpoint =="
GEARS_NO_WINDOW=1 \
GEARS_DRAW_FRAME_MIN_DRAWS="$GEARS_LAYER_MIN_DRAWS" \
GEARS_DRAW_FRAME_AFTER_GAMEPLAY=0 \
GEARS_DRAW_FRAME_COUNT=1 \
GEARS_DRAW_FRAME_NEEDS="$VS_HASH" \
GEARS_DRAW_FRAME_CAMERA="$FROZEN:$CAMERA_NEAR" \
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
# pair_score still passed because most background pixels remained correlated.
# Prove both that the runtime accepted the script and that the title polled far
# enough for at least one scripted transition to fire.
input_lines=$(grep -c '^\[input\]' "$OUT/ours.log" 2>/dev/null || true)
scripted_sources=$(grep -c '^\[input\] scripted input:' "$OUT/ours.log" 2>/dev/null || true)
scripted_steps=$(grep -c '^\[input\] scripted pad at ' "$OUT/ours.log" 2>/dev/null || true)
if [ "$scripted_sources" -ne 1 ] || [ "$scripted_steps" -lt 1 ]; then
    echo "REFUSING: native input validation scanned $input_lines input log line(s)," >&2
    echo "found $scripted_sources scripted-source declaration(s) and $scripted_steps" >&2
    echo "fired scripted step(s). The camera can match behind a modal, so this pair" >&2
    echo "cannot support a pixel or pass comparison." >&2
    grep '^\[input\]' "$OUT/ours.log" | head -5 >&2 || true
    exit 10
fi
echo "   native input validated: scanned $input_lines input log line(s), one scripted source, $scripted_steps fired step(s)"

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

# ------------------------------------------------------------------ the score
echo "== scoring the pair (it is measured, not assumed) =="
python3 "$REPO/tools/provenance.py" check "$OUT/ours" "$OUT/theirs" || exit 7
exec python3 "$REPO/tools/pair_score.py" --ours "$OUT/ours" --theirs "$OUT/theirs"

#!/bin/sh
# Walk the title from boot into an Act 1 gameplay frame and render frames with
# the guest-draw backend.
#
# The path through the menus is the point of this script: reaching a GAMEPLAY
# frame (~743 draws, the deferred UE3 pipeline) rather than the title screen
# (~170 draws, one EDRAM surface) needs a scripted controller walk, and that
# walk was previously reconstructed by hand every time it was needed.
#
#   START            leave the title screen
#   A                dismiss the "no storage device" dialog
#   B                back out of the profile prompt
#   A, A, A          campaign -> act/chapter -> difficulty -> begin
#   A, A, A, A       spare presses -- see below
#
# The times are wall-clock milliseconds and are generous: the script only
# advances when the guest polls XamInputGetState, so a slower run just fires
# each step later, it does not desynchronise.
#
# THE SPARE PRESSES ARE NOT PADDING. With six presses this walk stopped on the
# difficulty screen and stayed there: a press that lands during a menu
# transition is swallowed, and the run then reports frames of ~170 draws that
# look like a successful capture until you notice they are a menu. A press on an
# already-accepted screen is harmless -- the extra A presses are on the level
# after it has begun -- so the walk is padded until it reliably reaches
# gameplay. The check at the end of the run is the draw count: an Act 1 frame
# carries 800+ draws, a menu frame under 200.
#
# Usage: tools/capture_gameplay_frame.sh [seconds] [extra env...]
#   GEARS_DRAW_FRAME_AT / _COUNT / _REPORT_EVERY are respected if already set.

set -eu

SECONDS_TO_RUN="${1:-180}"
[ $# -gt 0 ] && shift

: "${GEARS_DRAW_FRAME_AT:=1500}"
: "${GEARS_DRAW_FRAME_COUNT:=0}"
: "${GEARS_DRAW_FRAME_REPORT_EVERY:=60}"
: "${GEARS_NO_WINDOW:=1}"
# The walk is shared with run.sh --menu-walk; keeping a second copy here is
# what let the two drift apart. See tools/menu_walk.sh.
. "$(dirname "$0")/menu_walk.sh"
: "${GEARS_INPUT_SCRIPT:=$GEARS_MENU_WALK}"
: "${GEARS_BUILD_DIR:=scratch/build}"
# The game's own files, extracted from the user's disc image (see README).
: "${GEARS_GAME_DIR:=scratch/game}"

export GEARS_DRAW_FRAME_AT GEARS_DRAW_FRAME_COUNT \
       GEARS_DRAW_FRAME_REPORT_EVERY GEARS_NO_WINDOW GEARS_INPUT_SCRIPT

# Kill only the process this script started, by PID -- never by name, because
# other runs of the same binary may be in flight.
"$GEARS_BUILD_DIR/runtime/gears1" "$GEARS_GAME_DIR/default.xex" "$GEARS_GAME_DIR" "$@" &
pid=$!
trap 'kill -9 "$pid" 2>/dev/null || true' EXIT INT TERM
sleep "$SECONDS_TO_RUN"
kill -9 "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

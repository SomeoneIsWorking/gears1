#!/bin/sh
# Drive the title, headless and reproducibly, from boot through its own menus
# into the campaign, far enough that it reaches a CHECKPOINT and mounts its save
# content. This is the repro for everything on the save path.
#
# The button sequence below was recovered from a run log after it had been lost:
# it existed only as an environment variable typed at a shell, so every session
# that wanted the save path had to rediscover it, and two runs were wasted
# proving that the title simply sits in the menu without it. That is what this
# file exists to prevent.
#
#   START  leaves the title screen           (0x0010)
#   A      accepts the main menu             (0x1000)
#   B      dismisses the storage dialog      (0x2000)
#   A x3   walks through act/chapter select  (0x1000)
#
# The script only advances when the guest POLLS the pad, so the timings are
# guest-observed rather than wall-clock -- which is what makes a headless run
# repeatable. The mount lands at roughly 65 s, a little after the last press.
#
# Usage: tools/run_to_checkpoint.sh <log file> [extra env assignments...]
set -e

log="${1:-scratch/logs/checkpoint.log}"
shift 2>/dev/null || true

game="${GEARS_GAME_DIR:-scratch/game}"
binary="${GEARS_BINARY:-scratch/build/runtime/gears1}"

if [ ! -d "$game" ]; then
    echo "no game data directory at '$game' -- set GEARS_GAME_DIR" >&2
    exit 1
fi

mkdir -p "$(dirname "$log")"

GEARS_INPUT_SCRIPT='25000:START,25300:,30000:A,30300:,35000:B,35300:,42000:A,42300:,50000:A,50300:,60000:A,60300:' \
GEARS_NO_WINDOW=1 \
    "$@" "$binary" "$game/default.xex" "$game" > "$log" 2>&1 &
pid=$!
echo "$pid"

# Stop once the content is mounted and the save path has had time to run, or
# when the title exits on its own. Killing BY PID and never by name: other runs
# of this same binary may be in flight.
waited=0
while [ "$waited" -lt 300 ]; do
    sleep 2
    waited=$((waited + 2))
    if grep -q "mounted as" "$log" 2>/dev/null; then
        # Keep watching well past the mount. The save is not written at mount
        # time -- the title mounts, then does its own work, and a window that
        # closes too soon reports "it never opened a file" when the truth is
        # "we stopped looking". Overridable for a longer soak.
        sleep "${GEARS_POST_MOUNT_SECONDS:-90}"
        break
    fi
    kill -0 "$pid" 2>/dev/null || exit 0
done
kill -9 "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

# DID IT ACTUALLY GET THERE? This script drives the title with a TIMED button
# script, and a timed script silently stops working the moment the menus change
# -- which is exactly what happened when the storage-device dialog stopped
# appearing: two runs were spent before anyone noticed the title was sitting in
# a menu rather than reaching the campaign. A run that did not reach the level
# is not a run with an interesting result; it is a broken instrument, and it has
# to say so rather than producing a quiet log that looks like evidence.
# The marker is the CONTENT MOUNT, not package opens: CookedXenon packages are
# loaded during boot as well, so a run that never left the menu still shows
# dozens of them (25 in the run that exposed this). The mount is the milestone
# this script exists to reach, it is logged at info so no debug channel is
# needed, and it is absent from every menu-only run.
reached=0
grep -q "mounted as" "$log" 2>/dev/null && reached=1
if [ "$reached" -eq 0 ]; then
    echo "run_to_checkpoint: THE TITLE NEVER REACHED THE LEVEL LOAD." >&2
    echo "  No save content was mounted, so the scripted input did not" >&2
    echo "  navigate the menus. Do NOT read this log as evidence about the" >&2
    echo "  campaign -- recalibrate GEARS_INPUT_SCRIPT first (the button" >&2
    echo "  sequence is timed, and it breaks whenever the menu flow changes)." >&2
    exit 3
fi
echo "run_to_checkpoint: reached the checkpoint ($(grep -c 'mounted as' "$log") content mount(s))" >&2

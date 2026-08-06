#!/bin/sh
# Build the runtime and play the title. The one entry point for "just run it".
#
# Everything it does is what the README spells out by hand -- configure, build,
# then launch the runtime with the game's data directory. It exists because
# those three steps were being retyped every session, and the second argument
# (the data directory) is easy to forget: without it the title quits early in a
# way that reads as a crash, so this script refuses to start rather than let
# that happen.
#
# Usage: ./run.sh [options] [-- extra runtime arguments]
#
#   --headless          no window (GEARS_NO_WINDOW=1); measurement runs
#   --no-build          run whatever is already built
#   --log <path>        tee the run's output here (default scratch/logs/run.log)
#   --script <steps>    scripted pad input, e.g. '25000:START,25300:'
#   --present-dump N    write the next N frames AS PRESENTED to scratch/screenshots
#                       (after frame 300). This is the only capture that goes
#                       through the swapchain blit -- every other screenshot this
#                       project takes comes from the renderer's readback, before it,
#                       which is how an sRGB swapchain washed out the whole window
#                       while every capture in the repo looked correct (catalog #60)
#   --menu-walk         the scripted walk from the title screen into Act 1
#   -h, --help          this text
#
# Environment (all optional):
#   GEARS_GAME_DIR      the title's data files, extracted from your disc
#                       (default scratch/game)
#   GEARS_BUILD_DIR     build directory (default scratch/build)
#   Every GEARS_* knob in docs/knobs.md is passed through untouched.

set -eu

self=$(dirname "$0")
cd "$self"

build_dir="${GEARS_BUILD_DIR:-scratch/build}"
game_dir="${GEARS_GAME_DIR:-scratch/game}"
log="scratch/logs/run.log"
build=1
headless=0
input_script="${GEARS_INPUT_SCRIPT:-}"
present_dump=""

# The walk that reaches Act 1 gameplay, kept in one place -- it is the same
# sequence tools/capture_gameplay_frame.sh and tools/run_to_checkpoint.sh use,
# and it only advances when the guest polls the pad, so it does not drift with
# machine speed. START, A (storage dialog), B (profile prompt), A/A/A (campaign,
# chapter, difficulty).
# THE WALK IS NOT DEFINED HERE. It lives in tools/menu_walk.sh because it was
# also copied into tools/capture_gameplay_frame.sh and the two drifted -- this
# copy stopped pressing at 60 s and no longer reached Act 1 at all, while still
# being described as the walk into Act 1. See that file.
. "$self/tools/menu_walk.sh"
menu_walk=$GEARS_MENU_WALK

usage() { sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case "$1" in
        --headless)  headless=1 ;;
        --no-build)  build=0 ;;
        --log)       log="$2"; shift ;;
        --script)    input_script="$2"; shift ;;
        --menu-walk) input_script="$menu_walk" ;;
        --present-dump) present_dump="$2"; shift ;;
        -h|--help)   usage; exit 0 ;;
        --)          shift; break ;;
        -*)          echo "run.sh: unknown option '$1' (try --help)" >&2; exit 2 ;;
        *)           break ;;
    esac
    shift
done

# REFUSE rather than run without the data. A run with no data directory opens no
# files, calls XamLoaderLaunchTitle and exits within seconds; that log looks like
# a crash and has been read as one.
if [ ! -f "$game_dir/default.xex" ]; then
    echo "run.sh: no '$game_dir/default.xex'." >&2
    echo "  The title's files must be extracted from your own disc image first:" >&2
    echo "    export GEARS_ISO=/path/to/your/Gears\\ of\\ War.iso" >&2
    echo "    python3 tools/gdf_extract.py --extract-all --out $game_dir" >&2
    echo "  Or point GEARS_GAME_DIR at an existing extraction. See README.md." >&2
    exit 1
fi

if [ "$build" -eq 1 ]; then
    if [ ! -f "$build_dir/build.ninja" ] && [ ! -f "$build_dir/Makefile" ]; then
        echo "run.sh: configuring $build_dir" >&2
        cmake -S . -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
              -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang >&2
    fi
    cmake --build "$build_dir" --target gears1 >&2
fi

binary="$build_dir/runtime/gears1"
if [ ! -x "$binary" ]; then
    echo "run.sh: '$binary' is not built (drop --no-build, or check the build)" >&2
    exit 1
fi

mkdir -p "$(dirname "$log")" scratch/screenshots

if [ "$headless" -eq 1 ]; then
    export GEARS_NO_WINDOW=1
fi
if [ -n "$input_script" ]; then
    export GEARS_INPUT_SCRIPT="$input_script"
fi
if [ -n "$present_dump" ]; then
    export GEARS_PRESENT_DUMP="$present_dump"
    : "${GEARS_PRESENT_DUMP_AT:=300}"
    export GEARS_PRESENT_DUMP_AT
    echo "run.sh: will write $present_dump presented frame(s) to scratch/screenshots" >&2
fi

echo "run.sh: $binary $game_dir/default.xex (log: $log)" >&2

# The log is a TEE, not a redirect: a run you are watching should still print to
# the terminal, and a run you want to grep afterwards should still leave a file.
#
# Through a FIFO rather than `| tee`, for two reasons a pipeline gets wrong:
#   - a pipeline's exit status is tee's, and tee always succeeds, so a runtime
#     that crashed would report success to every caller;
#   - a pipeline puts the runtime in a subshell, so it is a GRANDCHILD of this
#     script. Anything that kills run.sh then leaves the runtime running,
#     detached, still holding the GPU and the audio device. That has already
#     happened once.
# Here the runtime is a direct child whose PID we hold: its status is the
# script's status, and the trap takes it down with us. A SIGKILL of the script
# itself is the one case nothing can cover -- kill run.sh with TERM, not KILL.
fifo="$log.fifo"
rm -f "$fifo"
mkfifo "$fifo"
tee "$log" < "$fifo" &
tee_pid=$!

"$binary" "$game_dir/default.xex" "$game_dir" "$@" > "$fifo" 2>&1 &
pid=$!
trap 'kill -TERM "$pid" 2>/dev/null || true' INT TERM HUP

rc=0
wait "$pid" || rc=$?
wait "$tee_pid" 2>/dev/null || true
rm -f "$fifo"
exit "$rc"

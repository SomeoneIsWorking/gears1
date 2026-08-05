#!/bin/sh
# Measure how often a rare crash reproduces, by running several instances of the
# title CONCURRENTLY with separate save directories.
#
# WHY THIS EXISTS. #50 crashes on roughly one run in ten. Measuring that one run at
# a time costs three minutes per sample, so a usable sample is an hour of wall
# clock and a dozen tool calls -- and a run of clean results says almost nothing at
# that rate. Twelve consecutive clean runs at a true 10% is a 26% event, which is
# exactly the trap: it reads as "the bug is gone".
#
# Each instance gets its own XDG_DATA_HOME, because the title's content mount lives
# under it and concurrent runs sharing one would fight over the same save.
#
# THREE OUTCOMES, NOT TWO. A run that reaches the cap is not automatically a
# clean one: #44's failure is the guest ceasing to make progress while the
# process stays alive and the renderer keeps drawing, so `timeout` returns 124
# and an exit-code-only classifier calls it clean. It was counting the project's
# top blocker as a pass. runtime/wait_probe.cpp already detects and names that
# state, so this reads ITS verdict rather than guessing from a frame threshold.
#
# Usage: tools/repro_rate.sh [runs] [seconds] [parallel]
#
# The timeout is REPORTED in the summary, because a crash that happens later than
# the cap is counted as clean and the number is meaningless without it.
set -e

# --selftest: prove the STALLED classifier can actually produce a positive.
#
# It exists because the first real measurement returned "0 stalled" and that
# result was worthless: a classifier that has never fired cannot tell "no run
# stalled" from "this script cannot see a stall". So the shipping artifact
# carries a case that MUST come out STALLED and one that MUST NOT.
if [ "${1:-}" = "--selftest" ]; then
    t=$(mktemp -d "${SCRATCH:-scratch}/repro_selftest.XXXXXX")
    trap 'rm -rf "$t"' EXIT
    fail=0

    # Case 1: reached the cap, detector reported a stall -> must be STALLED.
    printf '[stall] stall detector armed: reports after 8 s\n' > "$t/a.log"
    printf '[draw] VdSwap: 1860 frames submitted\n' >> "$t/a.log"
    printf '[stall:warn] audio has made no progress for 9 s. The guest made 0\n' >> "$t/a.log"
    # Case 2: reached the cap, no stall reported -> must be clean.
    printf '[stall] stall detector armed: reports after 8 s\n' > "$t/b.log"
    printf '[draw] VdSwap: 4380 frames submitted\n' >> "$t/b.log"
    # Case 3: the "never started" wording, which is a different message and
    # must ALSO be caught -- it was written as a separate string in wait_probe.
    printf '[stall] stall detector armed: reports after 8 s\n' > "$t/c.log"
    printf '[stall:warn] xma has NEVER made progress in the 40 s since\n' >> "$t/c.log"

    for case in a:STALLED b:clean c:STALLED; do
        f=${case%%:*}; want=${case##*:}
        msg=$(grep -oE '[a-z_]+ has (made no progress for [0-9]+ s|NEVER made progress)' \
              "$t/$f.log" 2>/dev/null | head -1 || true)
        if [ -n "$msg" ]; then got=STALLED; else got=clean; fi
        if [ "$got" = "$want" ]; then
            echo "  ok   $f.log -> $got"
        else
            echo "  FAIL $f.log -> $got, expected $want"
            fail=1
        fi
    done
    if [ "$fail" = "0" ]; then
        echo "3 of 3 classifier cases pass"
        exit 0
    fi
    echo "the STALLED classifier is broken -- do not trust a rate from it"
    exit 1
fi

runs="${1:-8}"
seconds="${2:-170}"
parallel="${3:-4}"

game="${GEARS_GAME_DIR:-scratch/game}"
binary="${GEARS_BINARY:-scratch/build/runtime/gears1}"
script="${GEARS_INPUT_SCRIPT:-25000:START,25300:,32000:A,32300:,39000:A,39300:,46000:A,46300:,53000:A,53300:,60000:A,60300:,67000:A,67300:,74000:A,74300:}"

if [ ! -x "$binary" ]; then
    echo "no binary at '$binary' -- build first" >&2
    exit 1
fi
if [ ! -d "$game" ]; then
    echo "no game data at '$game' -- set GEARS_GAME_DIR" >&2
    exit 1
fi

outdir="scratch/logs/rate-$$"
mkdir -p "$outdir"

one_run() {
    n="$1"
    home="$outdir/home$n"
    mkdir -p "$home"
    # `set -e` IS OFF FOR THIS LINE ON PURPOSE. With errexit on, a non-zero exit
    # from timeout aborts the subshell BEFORE the exit code is recorded, so no
    # .exit file is written -- and the summary below, which treats a missing code
    # as "not 124", then reports every run as a crash. That is exactly what the
    # first version of this script did: it announced "8 of 8 runs crashed" when in
    # truth it had failed to record a single exit code. A measurement tool whose
    # failure mode is a confident wrong answer is worse than no tool.
    set +e
    XDG_DATA_HOME="$home" GEARS_INPUT_SCRIPT="$script" GEARS_NO_WINDOW=1 \
        timeout "$seconds" "$binary" "$game/default.xex" "$game" \
        > "$outdir/run$n.log" 2>&1
    rc=$?
    set -e
    echo "$rc" > "$outdir/run$n.exit"
}

# Launched in batches of $parallel, tracking PIDs so nothing is left orphaned and
# nothing is killed by pattern -- other agents and the operator run this same
# binary, so `pkill gears1` would take their runs down too.
pids=""
launched=0
n=1
while [ "$n" -le "$runs" ]; do
    one_run "$n" &
    pids="$pids $!"
    launched=$((launched + 1))
    if [ "$launched" -ge "$parallel" ]; then
        for p in $pids; do wait "$p" 2>/dev/null || true; done
        pids=""
        launched=0
    fi
    n=$((n + 1))
done
for p in $pids; do wait "$p" 2>/dev/null || true; done

crashed=0
clean=0
stalled=0
unobserved=0
n=1
while [ "$n" -le "$runs" ]; do
    rc=$(cat "$outdir/run$n.exit" 2>/dev/null || echo "")
    if [ -z "$rc" ]; then
        # NOT counted as a crash. A missing exit code means this script failed to
        # observe the run, which is a broken measurement and must be reported as
        # such rather than folded into either column.
        echo "  run $n: NOT OBSERVED -- no exit code was recorded, so this run"
        echo "      counts as neither clean nor crashed and the totals below are"
        echo "      short by one. Fix the harness before reading the rate."
        unobserved=$((unobserved + 1))
        n=$((n + 1))
        continue
    fi
    frames=$(grep -oE 'VdSwap: [0-9]+ frames' "$outdir/run$n.log" 2>/dev/null | tail -1 | grep -oE '[0-9]+' || true)
    restore=$(grep -c 'checkpoint restore:' "$outdir/run$n.log" 2>/dev/null || echo 0)
    # The stall detector's OWN verdict, not a frame-count threshold. A threshold
    # would need a healthy-frame number to compare against, and that number moves
    # with the host, the build and where the scripted input got to.
    stallmsg=$(grep -oE '[a-z_]+ has (made no progress for [0-9]+ s|NEVER made progress)' \
               "$outdir/run$n.log" 2>/dev/null | head -1 || true)
    if [ "$rc" = "124" ] && [ -n "$stallmsg" ]; then
        stalled=$((stalled + 1))
        echo "  run $n: STALLED (alive at the ${seconds}s cap, but not progressing)"
        echo "      $stallmsg"
        echo "      frames=${frames:-?} restore=$restore"
    elif [ "$rc" = "124" ]; then
        clean=$((clean + 1))
        echo "  run $n: clean (reached the ${seconds}s cap) frames=${frames:-?} restore=$restore"
    else
        crashed=$((crashed + 1))
        fault=$(grep -oE 'address: .*' "$outdir/run$n.log" 2>/dev/null | head -1 || true)
        ctx=$(grep -oE 'context: .*' "$outdir/run$n.log" 2>/dev/null | head -1 || true)
        echo "  run $n: CRASHED exit=$rc frames=${frames:-?} restore=$restore"
        [ -n "$fault" ] && echo "      $fault"
        [ -n "$ctx" ] && echo "      $ctx"
    fi
    n=$((n + 1))
done

echo
echo "$crashed crashed, $stalled stalled, $clean clean, $unobserved not observed, out of $runs runs"
echo "at ${seconds}s each, ${parallel} at a time."
if [ "$unobserved" -gt 0 ]; then
    echo "WARNING: $unobserved run(s) produced no exit code. The rate above is not"
    echo "trustworthy until that is fixed."
fi
echo "Logs in $outdir. The cap matters: a crash later than ${seconds}s counts as clean"
echo "here, so this is a lower bound on the true rate, not an estimate of it."
if [ "$stalled" = "0" ] && [ "$clean" -gt 0 ]; then
    # SAY WHAT THE ZERO MEANS. "0 stalled" is only evidence if the detector was
    # armed and had time to fire; a run shorter than its arming delay reports no
    # stall whether or not one happened.
    armed=$(grep -c 'stall detector armed' "$outdir"/run*.log 2>/dev/null | \
            awk -F: '{n+=$2} END{print n+0}')
    echo "0 stalled across $armed run(s) that armed the stall detector."
    if [ "$armed" -lt "$runs" ]; then
        echo "  WARNING: only $armed of $runs runs armed it, so the rest could not"
        echo "  have reported a stall either way."
    fi
fi

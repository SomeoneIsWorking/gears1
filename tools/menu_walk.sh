#!/bin/sh
# THE scripted pad walk from the title screen into Act 1 gameplay, defined ONCE.
#
# WHY THIS FILE EXISTS. This walk was copied into `run.sh --menu-walk` and into
# `tools/capture_gameplay_frame.sh`, and the two DRIFTED: the capture script
# kept pressing A at 75/90/105/120 s while run.sh stopped at 60 s. run.sh's own
# `--help` still claimed it was "the scripted walk from the title screen into
# Act 1", and it no longer was -- a 200 s headless run under it sat on a menu
# for all 5,481 frames, never exceeding 194 draws (Act 1 gameplay is 620-870).
#
# A stale walk does not announce itself. It produces a run that looks healthy in
# every way that is checked -- no crash, no stall, frames advancing, draws
# issuing -- while measuring the wrong screen entirely. Any session that used
# `run.sh --menu-walk` to "check the game in motion" was checking a menu.
#
# So: one definition, sourced by both.
#
#   . "$(dirname "$0")/menu_walk.sh"    # from anything in tools/
#   . tools/menu_walk.sh                # from the repo root
#
# Times are MILLISECONDS of guest run time. Each press is a button step
# followed 300 ms later by a release step -- the title ignores a button that is
# never released. The trailing presses past 60 s are not padding: the campaign
# load sits on screens that need dismissing, and dropping them is exactly the
# drift this file exists to prevent.
#
# An already-set GEARS_INPUT_SCRIPT wins, so a one-off override still works:
#   GEARS_INPUT_SCRIPT='25000:START,25300:' ./run.sh --headless --menu-walk

: "${GEARS_MENU_WALK:=25000:START,25300:,30000:A,30300:,35000:B,35300:,42000:A,42300:,50000:A,50300:,60000:A,60300:,75000:A,75300:,90000:A,90300:,105000:A,105300:,120000:A,120300:}"
export GEARS_MENU_WALK

# How long a run must last for this walk to MEAN anything. The last press is at
# 120 s and the level load follows it, so a run shorter than this reports "no
# gameplay" for the trivial reason that it stopped first -- which reads exactly
# like a title that cannot reach gameplay.
: "${GEARS_MENU_WALK_MIN_SECONDS:=180}"
export GEARS_MENU_WALK_MIN_SECONDS

# ---------------------------------------------------------------------------
# THE SAME WALK, INDEXED BY THE GUEST'S OWN FRAME COUNTER, FOR BOTH SIDES.
#
# WHY THIS EXISTS, AND WHAT IT FIXES. `tools/oracle_lockstep.sh` is built to put
# our frame N beside the oracle's frame N, and its header is right that the
# guest frame counter is the index that makes that meaningful. But the two walks
# it fed the two sides were two hand-written strings, and they were never the
# same walk:
#
#     ours   f150:START, then A at f300,420,540,660,780,900,1020,1140,1260
#     theirs START@150 REPEATING EVERY 270 FRAMES, A@300 REPEATING EVERY 120
#
# The oracle's repeats never stop. It goes on pressing START -- which is PAUSE
# once the level is up -- and A every 120 frames for the whole run, while our
# side stops pressing at f1260. So the two sides were driven differently for
# every gameplay frame either of them reached, and "frame N is the same game
# moment" was false by construction. Neither side has movement input at all, so
# neither walks anywhere. Every cross-side comparison this project has drawn
# from that script was between two different points in the game.
#
# So the walk is a TABLE, here, once, and the two notations are GENERATED from
# it. They cannot drift again without this table changing.
#
# Frames, not milliseconds: our runtime presents at a steady 30 Hz (measured --
# the millisecond walk above lands at f1800 for its 60 s press and f3600 for its
# 120 s press), but the oracle does not, and a level load stalls the frame
# counter on both sides while the wall clock runs on. Indexed by frames, a load
# that takes twice as long simply delays the next press instead of desyncing.
#
# Each entry is `<frame>:<name>`. A name is a BUTTON (START/A/B/X/Y), a held
# button (`START~120`), or a STICK
# deflection (LX/LY/RX/RY with +, - or 0, where 0 re-centres). Buttons are held
# briefly and released; a deflection is held until something touches that axis
# again -- the same semantics on both sides.
: "${GEARS_WALK_TABLE:=750:START 900:A 1050:B 1260:A 1500:A 1800:A 2250:A 2700:A 3150:A 3600:A 4500:LY+ 5100:LY0 5250:RX+ 5400:RX0 5550:LY+ 6150:LY0}"
export GEARS_WALK_TABLE

# How long a button is held, in guest frames. Must match the oracle driver's
# HoldTicks() (8) so a press is the same length on both sides -- a title that
# polls once a frame sees the same number of held frames either way.
: "${GEARS_WALK_HOLD_FRAMES:=8}"
export GEARS_WALK_HOLD_FRAMES

# OUR notation: "f<frame>:<name>", and a step sets the WHOLE pad state, so a
# button needs an explicit release step and a stick re-centre is a bare step.
gears_walk_ours() {
    _out=""
    for _e in $GEARS_WALK_TABLE; do
        _f=${_e%%:*}; _n=${_e#*:}; _hold=$GEARS_WALK_HOLD_FRAMES
        case "$_n" in *~*) _hold=${_n#*~}; _n=${_n%%~*} ;; esac
        case "$_n" in
            L[XY]0|R[XY]0) _out="$_out,f$_f:" ;;          # re-centre
            L[XY][+-]|R[XY][+-]) _out="$_out,f$_f:$_n" ;; # hold a deflection
            *) _out="$_out,f$_f:$_n,f$((_f + _hold)):" ;;
        esac
    done
    printf '%s' "${_out#,}"
}

# THE ORACLE's notation: "<name>@<frame>", no repeats -- a repeat is what made
# the two walks differ, and nothing in this table needs one.
gears_walk_theirs() {
    _out=""
    for _e in $GEARS_WALK_TABLE; do
        _f=${_e%%:*}; _n=${_e#*:}; _hold=$GEARS_WALK_HOLD_FRAMES
        case "$_n" in *~*) _hold=${_n#*~}; _n=${_n%%~*} ;; esac
        case "$_n" in
            L[XY]0|R[XY]0|L[XY][+-]|R[XY][+-]) _out="$_out,$_n@$_f" ;;
            *)
                _at=$_f; _last=$((_f + _hold - GEARS_WALK_HOLD_FRAMES))
                [ "$_last" -lt "$_f" ] && _last=$_f
                while :; do
                    _out="$_out,$_n@$_at"
                    [ "$_at" -ge "$_last" ] && break
                    _at=$((_at + GEARS_WALK_HOLD_FRAMES - 1))
                    [ "$_at" -gt "$_last" ] && _at=$_last
                done
                ;;
        esac
    done
    printf '%s' "${_out#,}"
}

# The last frame the table touches. A run shorter than this did not finish the
# walk, and reporting "no gameplay" from it would be reporting that it stopped
# early -- the frame-indexed twin of GEARS_MENU_WALK_MIN_SECONDS above.
gears_walk_last_frame() {
    _last=0
    for _e in $GEARS_WALK_TABLE; do
        _f=${_e%%:*}; _n=${_e#*:}; _end=$_f
        case "$_n" in *~*) _end=$((_f + ${_n#*~})) ;; esac
        [ "$_end" -gt "$_last" ] && _last=$_end
    done
    printf '%s' "$_last"
}

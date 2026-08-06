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

#!/usr/bin/env bash
# Which captures contain a SKINNED CHARACTER -- and the self-test that proves
# the detector can still say both answers.
#
# WHY THIS EXISTS. Catalog #77 needs a frame with a character in it, and the
# question "does this capture have one?" was answered by hand three times, each
# time by eyeballing the diag table's largest meshes and their vertex constants.
# That is slow, and it was wrong twice: it certified bright.gfr as the ONLY
# capture in the tree containing a skinned character draw, when four of the
# fifteen do.
#
# The detector is a property of the MICROCODE (see runtime/frame_content.h): a
# vertex shader that indexes its float constants through the address register is
# doing a bone-palette lookup, which rigid geometry has no reason to do.
#
#   tools/skinned_frames.sh              # the table
#   tools/skinned_frames.sh --list       # every skinned draw, with its size
#   tools/skinned_frames.sh --selftest   # must print PASS, exit 0
#
# THE SELF-TEST IS THE POINT OF THE THIRD MODE. A detector whose normal answer
# is "no" cannot be distinguished, from its output, from a detector that is
# broken -- so it is fed one capture that MUST come back positive and one that
# MUST come back negative, and a run where either is wrong fails loudly. Both
# files must exist: a missing corpus is a refusal, never a pass.
set -euo pipefail
cd "$(dirname "$0")/.."

replay=scratch/build/runtime/frame_replay
[[ -x $replay ]] || { echo "build $replay first (ninja -C scratch/build frame_replay)" >&2; exit 1; }

mode=${1:-table}

verdict() { # <capture> -> FOUND | NONE | UNAVAILABLE
    local rc=0
    GEARS_SKINNED_CHECK=1 "$replay" "$1" >/dev/null 2>&1 || rc=$?
    case $rc in
        0) echo FOUND ;;
        3) echo NONE ;;
        2) echo UNAVAILABLE ;;
        *) echo "ERROR($rc)" ;;
    esac
}

listing() { # <capture> -> the detector's explicit positive/negative lines
    local output rc=0 lines
    output=$(GEARS_SKINNED_CHECK=1 GEARS_SKINNED_CHECK_LIST=1 \
        "$replay" "$1" 2>&1) || rc=$?
    case $rc in
        0|3) ;;
        *) echo "REFUSING: frame_replay returned $rc while listing $1" >&2; return "$rc" ;;
    esac
    lines=$(printf '%s\n' "$output" | grep -E "skinned draw|no skinned draw" || :)
    [[ -n $lines ]] || {
        echo "REFUSING: detector produced no positive or negative line for $1" >&2
        return 1
    }
    printf '%s\n' "$lines"
}

if [[ $mode == --selftest ]]; then
    # The positive case: bright.gfr's draw 460 is the skinned character draw the
    # whole of catalog #77 is about. The negative: courtyard.gfr is a 744-draw
    # gameplay frame with no character in it, so a detector that answers "yes"
    # to any large frame fails here.
    pos=scratch/frames/bright.gfr
    neg=scratch/frames/courtyard.gfr
    for f in "$pos" "$neg"; do
        [[ -f $f ]] || { echo "REFUSING: $f is missing, so this self-test checked NOTHING" >&2; exit 1; }
    done
    vp=$(verdict "$pos"); vn=$(verdict "$neg")
    lp=$(listing "$pos"); ln=$(listing "$neg")
    echo "positive case $(basename "$pos"): expected FOUND, got $vp"
    echo "negative case $(basename "$neg"): expected NONE,  got $vn"
    # Exercise the same mixed-result loop as normal corpus mode. With `set -e`,
    # the former `[[ $v == FOUND ]] && (( ++found ))` exited on the first NONE,
    # so a nominal whole-corpus scan silently examined only act1.gfr. A unit
    # check of verdict() alone could never detect that control-flow failure.
    scanned=0; found=0
    for v in "$vp" "$vn"; do
        (( scanned += 1 ))
        if [[ $v == FOUND ]]; then (( found += 1 )); fi
    done
    if [[ $vp == FOUND && $vn == NONE && $scanned == 2 && $found == 1 &&
          $lp == *"skinned draw"* && $ln == *"no skinned draw"* ]]; then
        echo "PASS -- detector answers both ways and the corpus loop scans 2/2 (found 1)"
        exit 0
    fi
    echo "FAIL -- the detector is not discriminating; every verdict it has" >&2
    echo "       produced since it last passed is worthless" >&2
    exit 1
fi

captures=(scratch/frames/*.gfr)
(( ${#captures[@]} )) || { echo "REFUSING: no captures in scratch/frames -- this run examined NOTHING" >&2; exit 1; }

found=0
for c in "${captures[@]}"; do
    v=$(verdict "$c")
    if [[ $v == FOUND ]]; then (( found += 1 )); fi
    if [[ $mode == --list ]]; then
        printf '%-22s %s\n' "$(basename "$c")" "$v"
        listing "$c" | sed 's/^/    /'
    else
        printf '%-22s %s\n' "$(basename "$c")" "$v"
    fi
done
echo "$found of ${#captures[@]} captures submit a skinned character mesh."
echo "NOTE: 'FOUND' means the frame SUBMITS one, not that it is visible --" \
     "a submitted character can still be killed by clipping or colour-masked."

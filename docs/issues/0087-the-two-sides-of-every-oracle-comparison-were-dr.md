---
id: 87
title: The two sides of every oracle comparison were driven by different walks
status: open
symptom: oracle_lockstep.sh fed our runtime f150:START + A at ten fixed frames, and the oracle START@150+270,A@300+120 -- repeats that never stop -- so frame N was never the same game moment on the two sides
tags: oracle,tooling,workflow,gameplay-scene
created: 2026-08-07
updated: 2026-08-07
---

tools/oracle_lockstep.sh is built on the right idea and its header states it
correctly: both emulators count the guest's own VdSwap boundary, so indexing by
that counter makes frame N the same game moment on both sides. What it actually
did was feed the two sides two hand-written strings that were not the same walk.

    ours    f150:START, then A at f300,420,540,660,780,900,1020,1140,1260
    theirs  START@150 REPEATING EVERY 270 FRAMES, A@300 REPEATING EVERY 120

Our side stops pressing at f1260. The oracle's repeats never stop: it goes on
pressing START -- which is PAUSE once the level is up -- and A every 120 frames
for the whole run. Neither side has any movement input, so neither walks
anywhere deliberately. Two runs driven that differently are at different points
in the game at every gameplay frame, which is precisely the property the script
exists to provide.

The cost is not hypothetical. Two findings were built on comparisons made this
way and both are now withdrawn:

  * #86, "our Act 1 scene is rendered inset" -- the two frames were at different
    camera positions, and our side's "border" was rendered-but-dark geometry.
  * C023, "eight pixel shaders the console binds and we never do" -- a set
    difference over two runs that reached different points in the game measures
    how far each got. The oracle's frame 6000 has Marcus carrying a Lancer with
    312 rounds; ours is still in the unarmed prologue, which is the whole of the
    "missing HUD" that was read as evidence.

FIX. The walk is now a TABLE, in tools/menu_walk.sh, once:

    GEARS_WALK_TABLE="750:START 900:A 1050:B ... 4500:LY+ 5100:LY0 ..."

with `gears_walk_ours` and `gears_walk_theirs` GENERATING the two notations from
it, so they cannot drift again without the table changing. It is frame-keyed
because a level load stalls the frame counter on both sides while the wall clock
runs on, and it includes stick deflections so both sides actually walk.
oracle_lockstep.sh cross-checks that every frame the table names appears in both
generated strings, and PROVES that check fires on a deliberately mismatched pair
before trusting it to pass -- the first version of the check compared token
counts and flagged the correct walk as a mismatch, which is exactly the failure
mode a guard that is never exercised has.

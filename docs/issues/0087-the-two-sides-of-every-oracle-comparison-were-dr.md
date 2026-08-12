---
id: 87
title: The two sides of every oracle comparison were driven by different walks
status: open
symptom: oracle_lockstep.sh fed our runtime f150:START + A at ten fixed frames, and the oracle START@150+270,A@300+120 -- repeats that never stop -- so frame N was never the same game moment on the two sides
tags: oracle,tooling,workflow,gameplay-scene
created: 2026-08-07
updated: 2026-08-12
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

### Note (2026-08-12)
THE WALKS NO LONGER HAVE TO MATCH. This entry is a real tooling defect and its diagnosis is correct -- the two sides were fed different input schedules, so frame N was never the same game moment. But the requirement it exists to enforce, that both sides be driven identically so their frames correspond, is no longer the only way to make a comparison. Matching on the VIEW-PROJECTION does it directly: it is guest data both emulators carry, so two frames carrying the same one are the same moment regardless of how either side got there, and the two runs may be driven completely differently or not driven at all. GEARS_DRAW_FRAME_CAMERA and tools/camera_match.py implement it and are validated in both directions. THE PROOF THAT THIS IS ENOUGH is that a camera-gated capture and an oracle run with no shared schedule resolved THE SAME 16 PASSES with none only-ours and none only-theirs -- the first paired capture to achieve that -- where every content-predicate capture before it reported structural mismatches that were really two different moments. Fixing the walk mismatch is still worth doing for any purpose that needs the two sides to follow the same route, but it is no longer a prerequisite for comparing them.

### Note (2026-08-12)
HOW WELL EACH PAIRING METHOD ACTUALLY PAIRS, MEASURED FOR THE FIRST TIME. A properly-provenanced paired capture (scratch/paircap, both sides stamped with one pair id by tools/layer_capture.sh, 234 s, gpuguard clean) lets the content selector be scored rather than trusted. Our front-buffer resolve against EVERY console front-buffer candidate in the dumped window, log-luminance correlation, best over flips and shifts to +/-64 px: f875 0.389, f876 0.493, f877 0.383, f878 0.242, f879 0.229, f880 0.228, f881 0.199, f882 0.146, f883 0.125, f884 0.117, f885 0.107, f886 0.100. Twelve candidates scored, so 'none passed' is distinguishable from 'none were tried'. THE POSITIVE CONTROL -- our frame.ppm against our own front-buffer resolve, the same metric on the same quantization -- is 0.939. SO THE BEST AVAILABLE CONSOLE FRAME REACHES 0.49 WHERE A GENUINE MATCH REACHES 0.94, and the gate is 0.60. The content selector (GEARS_LAYER_AFTER frames past the first with GEARS_LAYER_MIN_DRAWS draws, applied identically to both sides) does NOT land the two emulators close enough for a pixelwise comparison. THE SHAPE OF THE CURVE IS THE EVIDENCE THAT THE METRIC WORKS: the scores fall off smoothly and monotonically either side of f876, and the best-fitting shift grows steadily with temporal distance (dy=0 at the peak, then 8, 16, 32, 40, 40 as the frames get further away) -- exactly what a moving camera produces. This is temporal proximity being measured correctly, not noise. f876 is a genuine interior peak, not a window boundary, so the closest moment IS in the window and is still only 0.49; our frame most likely falls BETWEEN console frames and the camera moves far enough per frame that even the nearest one cannot do better. WHAT THIS MEANS FOR EVERY 'match' VERDICT layer_compare HAS EVER PRINTED: it reports MEANS, and a mean agrees between two different moments of the same scene -- on this very pair it called srcC400 f32 a match at 0.0033 against 0.0035 while the two frames correlate at 0.49. A mean-based match verdict is not evidence that the two sides rendered the same moment, and it never was. THE FIX IS THE CAMERA GATE, NOT THE CONTENT SELECTOR, and it must run against the SAME oracle run: dump the oracle's resolves AND its vs_consts in one run, then drive our side camera-gated to a constants file from THAT run, stamping both with one pair id. That is a script, not a research question.

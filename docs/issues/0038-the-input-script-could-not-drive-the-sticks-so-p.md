---
id: 38
title: The input script could not drive the sticks, so playability was untestable
status: resolved
symptom: GEARS_INPUT_SCRIPT accepted only button names; Gears moves and aims on the analogue sticks, so no scripted run could exercise player movement at all
tags: input,harness,testing,gameplay
created: 2026-07-28
updated: 2026-07-28
---

FIXED. GEARS_INPUT_SCRIPT now accepts stick deflections: LX/LY/RX/RY suffixed
with '+' or '-', Y positive up as the console has it. Verified on a real run --
"6000:LY+,8000:LX-&A" publishes L(0,32767) then L(-32767,0) with button 0x1000.

The combiner changed from '+' to '&'. That is not cosmetic: '+' was already the
stick SIGN, so "LY++A" could split either way, and the first implementation
silently parsed "LY+" as the button "LY", warned about an unknown button, and
published a zero stick. A script that reads as though it walks forward and
actually does nothing is worse than one that fails.

WHY IT MATTERED: every scripted run in this project's history has been
button-only, which is enough to walk the menus and reach gameplay but cannot
press a direction. So "does the game respond to a player?" had never been asked,
let alone answered, and the harness could not have asked it.

STILL NOT ANSWERED, and recorded as such: whether the title responds to the stick.
A run holding LY+ for 40 s through Act 1 shows 230 of 278 consecutive reported
frames differing substantially -- but that section is cutscene-heavy and animates
on its own, so continuous change is NOT evidence of player control. Two runs
cannot be differenced either, because they are not frame-synchronous.

What IS proven: the input reaches the guest. The script only advances when the
guest polls XamInputGetState, so a step firing is itself proof of the poll, and
the stick values reach the published pad state.

A decisive test needs a causal signal rather than a difference -- the most
promising is Gears' own tutorial prompt ("MOVE: To follow Dominic, use <stick>"),
which the title dismisses once the player actually moves. Its disappearance under
a held stick, and persistence without one, would settle it.

### Resolution (2026-07-28)
ANSWERED, with a control arm: THE TITLE RESPONDS TO THE PLAYER. Two runs identical except for one step -- one holds the left stick forward from 200 s, the other presses nothing. In the stick run, 172 of 173 reported frames after the press show the scene changed (the camera translated forward: same wall, same barred windows, same electrical box, seen from much closer, and the cinematic letterbox gone). In the control run, 0 of 129 frames over the same window show it. The change is caused by the input, not by time -- which was the confound that made the first, difference-based attempt worthless. The runs also diverge in length (4160 vs 3720 rendered frames), consistent with the stick run progressing further through the level. WHAT IS NOT CLAIMED: that movement is faithful (no console to compare against), that other controls work (only the left stick was exercised), or that the game is completable. What is established is that guest input reaches the title AND the title acts on it -- so 'does the game work' in the sense a player means has its first real evidence rather than an assumption.

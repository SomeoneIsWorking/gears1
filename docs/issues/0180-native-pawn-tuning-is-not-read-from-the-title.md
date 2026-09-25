---
id: 180
title: Native pawn tuning is not read from the title
status: open
symptom: the native player moves with Unreal's stock pawn values (radius 34, half height 72, gravity -520, step 35) rather than Gears 1's
tags: native-engine,game,movement
state_items: S022
created: 2026-09-25
updated: 2026-09-25
---

`game::PawnTuning` holds Unreal's stock pawn and world values. The title's own
values live in the default objects of its pawn classes (collision cylinder
size, ground speed, run speed) and of its WorldInfo (gravity).

Next: read class default objects (the `Default__<Class>` export of the class's
package, falling back through superclass defaults), find the player pawn class
the game type spawns, and build `PawnTuning` from its defaults.

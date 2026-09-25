---
id: 180
title: Native pawn tuning is not read from the title
status: open
symptom: the native player moved with Unreal's stock pawn values (radius 34, half height 72, gravity -520, step 35) rather than Gears 1's
tags: native-engine,game,movement
state_items: S022
created: 2026-09-25
updated: 2026-09-25
---

`object::ClassDefaults` reads a class's `Default__<Class>` export, and each
superclass's default, from the class's package. It reads default subobjects
(`Default__<Class>.<Name>`) the same way. A property comes from the most
derived default that stores it.

`game::ReadPawnTuning` builds the player's `PawnTuning` from those defaults.
`gears_native` reads it for the pawn class that
`WarfareGame.WarGameSP`'s `DefaultPawnClass` names.

Values measured on the retail disc (inspector, `WarfareGame.xxx` and `Engine.xxx`):

| value | stored by | measured |
|---|---|---|
| player pawn class | `Default__WarGame.DefaultPawnClass` | `Pawn_COGMarcus` |
| collision radius | `Default__Pawn_COGGear.CollisionCylinder` | 34 |
| collision half height | `Default__Pawn_Infantry.CollisionCylinder` | 72 |
| ground speed | `Default__WarPawn` | 300 |
| roadie-run modifier | `Default__WarSpecialMove_RoadieRun.SpeedModifier` | 1.5 |
| max step height | `Default__WarPawn` | 40 |
| rotation rate (yaw) | `Default__WarPawn` | 50000 units/s |
| acceleration | `Default__Pawn` | 2048 |
| walkable floor Z | `Default__Pawn` | 0.7 |
| default gravity | `Default__WorldInfo.DefaultGravityZ` | -750 |

Remaining work:
- The code is not built yet, so the reader has no test.
- A level's own WorldInfo may override `DefaultGravityZ`. The level instance
  is not read yet; only the class default is.
- `BackwardMovementSpeedPercentage` (0.75) is not applied.

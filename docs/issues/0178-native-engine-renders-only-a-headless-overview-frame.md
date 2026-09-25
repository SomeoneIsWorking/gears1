---
id: 178
title: Native engine game loop, window and player camera
status: open
symptom: the native engine had no window, player, or frame loop; it only wrote headless overview images
tags: native-engine,render,window
state_items: S022
created: 2026-09-25
updated: 2026-09-25
---

`gears_native <cooked content directory> <persistent level>` is the game
executable: it loads the level with its streamed sublevels, builds collision
from BSP and collision-flagged static mesh sections, spawns the player at the
first PlayerStart, and runs a fixed-tick loop with walking movement, an
over-the-shoulder camera, and keyboard, mouse and gamepad input, presented
through an SDL3 window and Vulkan swapchain. Written but not yet built or run.

Remaining: the player has no visible body (skeletal meshes, 0176); pawn tuning
is Unreal's stock values, not the title's (0180); the level is chosen on the
command line rather than by the front end's flow; `run.sh` does not launch it.

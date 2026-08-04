---
id: 56
title: Rendering inside the guest's VdSwap halved the game's frame rate
status: resolved
symptom: In-game frame rate collapses: VdSwap 10.5 fps during gameplay against 29.9 in menus, and the audio pump falls behind with it
tags: gpu,draw,performance,threading,audio
created: 2026-08-04
updated: 2026-08-04
---

## Symptom

Gameplay ran at 10.5 VdSwap frames/s while the menus ran at 29.9, and the audio
pump -- which is paced by a hand-off with a guest thread (#43) -- fell behind at
the same moment.

## Cause

The command processor executes the guest's swap packet from inside the guest's
own VdSwap call, and the whole-frame render happened right there. A gameplay
frame costs 45-75 ms to record and submit, so the guest's render thread sat in
VdSwap for that long every frame. Everything the guest does downstream of the
swap -- including the audio ping-pong -- was paced by our renderer.

## Fix

runtime/render_thread.{h,cpp}: the frame's draw list is handed to a render thread
and the command processor returns. A frame that arrives while the renderer is
still busy is DROPPED (and counted -- a silent drop makes a renderer at half rate
look like one keeping up). Capture and measurement runs (GEARS_DRAW_FRAME_COUNT>0)
still render in line, because there the render IS the measurement.

The renderer reads guest memory while the guest writes the next frame into it.
That race exists on hardware too, where titles handle it with fences; here it can
mix two frames' data but cannot fault, since every read is bounds-checked. The
register snapshots are shared_ptr copies and the microcode lives in a map whose
entries are never erased, so only pixel data races.

The thread is NICED DOWN (+5). It is the only work in the process that may be
skipped; the audio mixer's 187.5 Hz hand-off may not.

## Measured, at machine load ~11

| | before | after |
|---|---|---|
| VdSwap during gameplay | 10.5 fps | 27.8-28.5 fps |
| rendered frames | 10.5/s (all) | 7-8/s, ~20/s dropped |
| RenderFrame wall | 63-75 ms | 105-126 ms (now competing with the guest) |

## NOT measured, and it matters

The nice-down and its effect on the audio pump were run at machine load 39 (other
projects building on the same box), where VdSwap read 5-6 fps and the pump 45-64
Hz. Those numbers say nothing about this change. Re-run on an idle machine before
concluding anything about audio under gameplay.

## Where the render time goes (live gameplay, load ~11)

draw loop ~85 ms = state+pipeline 25 + uniforms 16 + record 37 (of which
descriptor writes 33, of which "texture upload" 28) + index prep 2.

The 28 ms attributed to texture upload uploads NOTHING in a steady frame: it is
the staleness check (#53). Split out with its own counters: 15.55 MiB re-hashed
in 15.4 ms over 5224 bindings -- about 1 GB/s, so it is bound by reading cold
guest memory, not by the hash (XXH3 does 15 MiB in 0.3 ms warm).

NEXT LEVERS, in size order: descriptor-set caching across frames (33-52 ms/frame
of descriptor writes for bindings that mostly do not change), and page-level write
tracking to replace the content hash (15 ms/frame).

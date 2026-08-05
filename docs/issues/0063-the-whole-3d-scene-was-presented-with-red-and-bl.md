---
id: 63
title: The whole 3D scene was presented with red and blue exchanged: the scanout copy's copy_dest_swap was not applied to the presented surface
status: resolved
symptom: Gameplay looks cold grey-blue-green instead of Gears' rust palette; menus look right
tags: gpu,draw,colour,resolve,present,blocker
created: 2026-08-05
updated: 2026-08-05
---

## The measurement that found it

Per-channel means of one gameplay frame, comparing what we PRESENT with the
resolve destination -- the buffer the guest actually scans out:

    the scanout resolve   R 0.0991  G 0.0981  B 0.0708      (warm)
    what we presented     R 0.0708  G 0.0981  B 0.0991      (cold)

The same three numbers with red and blue exchanged. A rust-coloured game rendered
cold grey-blue.

## Cause

The guest renders into EDRAM in one channel order and asks for the exchange on the
way to memory: `copy_dest_swap`, bit 24 of RB_COPY_CONTROL. Our compute resolve
applies it correctly -- which is why the resolve dumps were right all along -- but
the PRESENTED image is taken from the surface itself, upstream of that copy, so it
never got the swap. Every 3D frame reached the screen in the wrong channel order.

Menus and the title screen were unaffected because their surface resolves without
the swap, which is why the reported screenshots showed correct menus and wrong
gameplay -- the asymmetry that finally pointed here after two days spent on
whole-frame explanations (swapchain format, colour space, compositor) that could
never have produced it.

## Fix

The frame's front-buffer resolve now tells the presented image what to do: when
that copy swaps, the surface goes into the present stage through a raw
`vkCmdCopyImage` from R8G8B8A8_UNORM into a B8G8R8A8_UNORM stage. A copy between
size-compatible formats moves bytes without conversion, so the channels land
exchanged -- the guest's copy_dest_swap, for free, with no shader and no per-pixel
work.

## Verified

Same captured Act 1 frame, replayed: the presented frame is now
R 0.0991 G 0.0981 B 0.0708, matching the scanout resolve to four decimals, and the
image is warm sepia concrete instead of cold blue-grey
(`scratch/screenshots/rb_swap_fixed.png`).

## What it cost to find

Every "the renderer's output is correct" in this session was a judgement of a dim
scene against no reference, and every one was wrong: the frames I was checking had
red and blue exchanged the whole time. The measurement that settled it took one
command -- per-channel means of our output against our own resolve dump -- and
was available from the first hour.

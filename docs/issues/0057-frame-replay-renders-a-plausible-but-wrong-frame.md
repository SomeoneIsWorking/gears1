---
id: 57
title: frame_replay renders a plausible but wrong frame: the capture's stale mirror size silently drops 606 of 722 draws
status: resolved
symptom: A replayed gameplay frame looks washed out and empty of world geometry, while the same frame rendered live looks correct
tags: instrument,replay,gpu,draw,trap
created: 2026-08-04
updated: 2026-08-04
---

## Symptom

`frame_replay scratch/frames/act1.gfr` renders a white/grey wash with only the
title card and some post-process haze -- no world geometry. The same frame in a
live run renders the prison interior correctly. The replayed image looks like a
FRAME, not like an error, which is what made it dangerous: it was read as
evidence that the renderer's colour path was blowing out.

## Cause

A capture stores `guestPhysicalMirrorBytes` -- how much guest physical memory the
translated shaders may fetch through -- as it was when the frame was recorded.
`act1.gfr` was captured on 27 July, while that mirror was 64 MiB. It was raised
to 512 MiB the same week (catalog #30, the same symptom in the live runtime), but
the capture still carries 64, and the replay honoured it: 606 of 722 draws fetch
past the mirror, read ZERO, and every primitive collapses at clipping.

The mirror is a property of the RENDERER, not of a frame. Freezing it into the
capture means every capture predating a renderer change replays a different
frame from the one the runtime renders -- silently, and forever.

## What it cost

Every replay-based comparison in the session of 4 August ran with 84% of the
frame's draws collapsed. The A/Bs remain valid as A/Bs (both arms had the same
input), but any judgement about how the frame LOOKED was worthless, and one was
made: the washed-out image was diagnosed as a tonemap/exposure defect. It is not
-- with the mirror corrected the frame renders the scene.

## Fix

- `gears::kGuestPhysicalMirrorBytes` in `runtime/gpu_draw.h` is now the single
  definition, used by the runtime and by the replay.
- `frame_replay` uses the RUNTIME's current value, not the capture's, and warns
  when the capture disagreed. `GEARS_REPLAY_MIRROR_MB` still overrides, which is
  what the knob was for.
- "frame geometry reach" is a WARNING when any draw fetches past the mirror, and
  says THE FRAME IS MISSING WORLD GEOMETRY. It was an info line among forty
  others; it had been printing the truth all along.

## Verified

Same capture, no environment: 722 of 737 draws issued, 0 past the mirror, and the
frame renders the scene (ceiling, corridor, lighting, the ASHES card) instead of
the wash.

## The instrument lesson

`frame_replay` is the renderer's primary instrument and it produced a
confident, plausible, wrong picture for a week. What made it wrong was not a bug
in the replay -- it was replaying exactly what it was told -- but a captured
COPY of a value that belongs to the code. Any capture format that stores
renderer configuration has this shape of failure in it. The rest of
`FrameCapture` should be audited for other frozen configuration: `guestWindowBytes`
is the obvious next one.

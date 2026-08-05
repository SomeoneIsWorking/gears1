---
id: C009
kind: claim
status: holds
created: 2026-08-05
tags: gpu,draw,clip
depends: runtime/gpu_draw_vertexfetch.cpp, tools/clip_volume_check.py
---

## Claim

The renderer clips draws 286/287 of courtyard.gfr correctly: they are outside the frustum in the guest's own vertex constants, so catalog #74's grey window slabs are not a clipping defect

## Evidence

GEARS_DRAW_VDUMP shows all six window draws fetch the same vertex buffer 0xe585000; GEARS_DRAW_VS_CONSTS shows their world matrices differ (translations 20-40k units apart); tools/clip_volume_check.py puts all 4 dumped vertices of 286 behind the camera (w<0) and of 287 at ndc.x -5.4..-3.8, while 288 -- the draw the pipeline statistics credit with 87 primitives and 90716 fragments -- comes out inside, which is the check that validates the assumed constant layout

## What would falsify it

if the real Xbox 360 output shows content in those two window openings on this exact frame, then the guest submitted draws we are not seeing and the clip is not the whole story

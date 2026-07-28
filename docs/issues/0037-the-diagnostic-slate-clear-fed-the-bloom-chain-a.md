---
id: 37
title: The diagnostic slate clear fed the bloom chain and hazed the whole frame
status: resolved
symptom: the rendered gameplay frame is soft and hazed, as though a fog sits over the whole image; surfaces are cleared to a dark slate (0.05,0.05,0.08) rather than to the guest's colour
tags: gpu,draw,draw-backend,clear,bloom,hdr,gameplay
created: 2026-07-28
updated: 2026-07-28
---

FIXED and MEASURED. The colour clear was a HOST DIAGNOSTIC -- a dark slate chosen
so that any lit pixel was obviously guest geometry -- and it is not black. Every
EDRAM surface began each frame lifted off zero by (0.05, 0.05, 0.08), INCLUDING
the 7e3 HDR surface the tonemap and bloom passes sample. A constant non-zero
floor under a bloom chain is a haze over the entire image, which is exactly what
the frame showed.

Like the depth clear, the guest's colour clear rides on a copy draw:
RB_COPY_CONTROL bit 8 is color_clear_enable, value in RB_COLOR_CLEAR /
RB_COLOR_CLEAR_LO. It is now honoured.

ONLY THE ZERO CASE IS HONOURED, and that is a deliberate limit rather than an
oversight. Across every captured run of this title -- 478 colour clears -- the
programmed value is 0x00000000 every single time, without one exception. A
per-format unpack of RB_COLOR_CLEAR would therefore produce identical output
whether it were correct or not, so writing one would be shipping code this data
cannot test. A non-zero value is COUNTED and REPORTED and falls back to the
previous behaviour instead of being decoded on a guess. If a frame that clears to
something else ever turns up, the report says so and the unpack can be written
against it.

MEASURED on the captured Act 1 frame: the presented frame changes on 2303227 of
2764816 bytes; mean luminance is essentially unchanged (22.53 -> 22.44) while
standard deviation RISES from 19.68 to 23.23 -- the signature of removing an
additive floor rather than of darkening. Pixels non-black falls from 97.9% to
95.5%, which is the slate-only pixels becoming properly black. Visually the haze
is gone and the scene is crisp: carved ceiling detail, stone wall texture, and
light shafts through the barred window that were previously washed out.

GEARS_DRAW_SLATE_CLEAR=1 restores the sentinel as a control arm; it remains
useful for telling guest geometry from an unwritten surface.

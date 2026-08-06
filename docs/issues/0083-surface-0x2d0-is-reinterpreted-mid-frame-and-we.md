---
id: 83
title: Surface 0x2d0 is reinterpreted mid-frame and we store values where EDRAM stores bits
status: investigating
symptom: a draw blends against 1.0 where the console reads 31.875; frame-wide red/green imbalance; the same EDRAM base declared under four bit layouts in one frame
tags: gpu,draw,edram,format,reinterpret,colour,act1,native-renderer
created: 2026-08-06
updated: 2026-08-06
---

## What was measured

An EDRAM base is a location, not a typed image. Surface `0x2d0` is declared
under five colour formats inside one Act 1 frame (`walk_gameplay.gfr`), and
`StorageColorFormat` collapses those to four distinct BIT layouts:
`k_8_8_8_8` x289, `k_2_10_10_10` x2, `k_2_10_10_10_FLOAT` x26+47, `k_16_16` x1.
The order, from `GEARS_DRAW_DIAG` with resolve rows excluded:

    draw 612 fmt 3   draw 613 fmt 4   draw 615 fmt 3   draw 616 fmt 12
    draw 640 fmt 0   draw 649 fmt 2   draw 650 fmt 12  draw 658 fmt 0
    draw 660 fmt 2   draw 662 fmt 12  draw 684 fmt 3   draw 716 fmt 0

Draw 649 is a full-screen `triangle_strip`, colour mask 15, 921600 fragments
shaded, no blend, `color_fmt` 2 = `k_2_10_10_10` -- a FIXED-POINT target, so the
hardware clamps its source colour and stores 10-bit UNORM bits. Draws 650, 651
and 655 declare `k_2_10_10_10_FLOAT_AS_16_16_16_16` on the same base and BLEND
(`blend_on` 1). On the console they read the bits `0x3FF` back as the 7e3 float
**31.875**; this renderer stores the value `1.0` it clamped to and reads `1.0`.

That closes the order question the previous session left open ("NOT YET
ESTABLISHED: that this frame's later draws actually reinterpret base 0x2d0 as a
float format after draw 649"). They do, immediately, and they blend against it.

## The pass, and what it does to the frame

`runtime/gpu_draw_reinterpret.cpp` + `runtime/shaders/edram_reinterpret.comp`
convert a surface's contents at every storage-format change: pack the stored
value under the old format, unpack under the new. `GEARS_DRAW_REINTERP=1`.
Bit layouts ported from Xenia's ownership transfer, 7e3 from its SPIR-V
translator. `GEARS_DRAW_REINTERP_SELFTEST=1` passes 5 of 5 on this GPU,
including the pair that must NOT change anything and a two-part round trip
(7e3 -> 8888 -> 7e3, the second half fed the first half's measured output),
which is the property that makes converting the WHOLE surface sound.

On `walk_gameplay.gfr`: 8 conversions, 2 refused (the `k_16_16` pair -- named,
not approximated). `GEARS_DRAW_AB=DRAW_REINTERP` puts the first divergence at
guest draw 640, the frame's first storage-format change.

`tools/frame_stats.py --diff`:

                      OFF                     ON
    R mean          0.0591                 0.3867
    G mean          0.0768                 0.3871
    B mean          0.0764                 0.3843
    R/G             0.7687                 0.9990
    p99             0.267                  1.000

**Catalog #62's headline -- "red is 78% of green frame-wide" -- disappears.**
The colour balance lands on the diagonal. And the picture blows out: large
regions saturate to white, with red/magenta fringing where the channel fields
of one format land across another's boundaries.

## What is NOT established

Whether the blow-out means the pass is wrong or means the pass is right and is
AMPLIFYING an upstream error. The mechanism is a multiplier: any pixel this
frame writes as 1.0 under a fixed-point format becomes 31.875 to the next
draw that blends. If our draw 649 shades brighter than the console's, the
reinterpretation turns a small error into a 31.875x one.

The round-trip self-test rules out the pass being destructive on untouched
pixels. What it cannot rule out is the pass being applied where the console
would not apply it -- it converts the whole surface at the format change, where
the hardware only reinterprets on a READ.

Next: the Xenia oracle renders this scene with a faithful per-(base, format) RT
cache. If its frame at the same moment is not blown out, the fault is upstream
of the reinterpretation, and #62 and #81 are the places to look.

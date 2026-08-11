---
id: 93
title: The diag table's scissor columns do not match the scissor the renderer set
status: investigating
symptom: GEARS_DRAW_DIAG reports sc_w x sc_h = 1280x720 for a draw whose pd.scissor.extent is 1280x512
tags: instrument,diag,scissor,msaa
created: 2026-08-11
updated: 2026-08-11
---

## What was measured

On `walk_gameplay.gfr`, diag draw 291 (a 2X triangle_list into surface 0x400):

    a lucent::info printed from the prepare loop, in the SAME run:
      PROBE291 gv sc 1280x512 -> pd.scissor 1280x512 (sx 1 sy 1 SW 1280 SH 720)

    the row GEARS_DRAW_DIAG wrote for draw 291, same run:
      sc_x 0  sc_y 0  sc_w 1280  sc_h 720

The guest's own scissor for that draw is 1280x512 -- a predicated tile band --
and the renderer sets exactly that. The table says 720.

Draws 290 and 292 print 1280x720 too, so it is not one row.

## What has been ruled out

* Column misalignment: every row of the file has 53 fields, header included,
  and the field the value was read from is `sc_h` by name in both a
  `csv.DictReader` and a positional `awk` read.
* The writer: it is `<< '\t' << pd.scissor.extent.height`, four fields after
  `pd.viewport.height`, which prints the right value (720) on the same row.
* Two entries with one diag index: `awk -F'\t' '$1=="291"'` returns ONE row,
  and `pd.diagIndex` is `&d - in.draws.data()`, unique per guest draw.
* A second frame: the probe printed once in the run that wrote the file.
* GEARS_DRAW_FIXEDVP, which would overwrite the scissor: unset.
* An overwrite elsewhere: `pd.scissor` is written in exactly one place
  (gpu_draw.cpp) and read in one (vkCmdSetScissor).

## Why it matters, beyond this draw

The scissor column is how a draw's coverage is reasoned about, and it was used
one step earlier in this session to size the EDRAM sample grid -- "every 2X
draw scissors the full 1280x720, so the grid needs 1440 rows". The real
scissors are 512-row bands. The grid size happens not to depend on it (it is
H * maxScaleY), but the reasoning did, and any other conclusion drawn from
that column is now suspect until this is explained.

## Next

Print `pd.scissor` from inside the table writer itself, for one named diag
index, in the same run as the prepare-time probe. That separates "the value
changed between prepare and report" from "the row is about a different entry"
-- the only two possibilities left.

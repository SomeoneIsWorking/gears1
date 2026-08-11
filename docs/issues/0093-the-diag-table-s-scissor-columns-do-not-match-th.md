---
id: 93
title: The diag table's scissor columns do not match the scissor the renderer set
status: resolved
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

### Note (2026-08-11)
NOT AN INSTRUMENT DEFECT. The table was telling the truth; the scissor really
does change between prepare and report, and the code that changes it is the
predicated-tile collapse.

The check this entry asked for -- print pd.scissor from inside the table writer,
in the same run as the prepare-time probe -- says:

    PROBE291-PREPARE  gv sc 1280x512 -> pd 1280x512
    PROBE291-TABLE    row 291 diag 291 scissor 0,0 1280x720

So the value changes after the prepare loop, and a grep for `.scissor` across
the whole runtime (rather than one file, which is what missed it) finds the
writer: gpu_draw_untile.cpp, CollapseEdramTiling.

    prepared[i].scissor.extent.height =
        std::max(prepared[i].scissor.extent.height,
                 uint32_t(dstBottom) - prepared[i].scissor.offset.y);

The guest replays the same geometry once per predicated tile with a window
offset; that pass collapses the replays into the base tile and WIDENS the base
tile's scissor to the union of the tiles' resolve destinations. 512 is the
guest's band, 720 is the union. The table prints the final value, which is the
one that was used.

AND IT WAS THE BLOCKER ON THE SAMPLE MODEL, which is why this was worth
chasing. dstBottom is a row of the resolve DESTINATION, in PIXELS; under
GEARS_DRAW_MSAA the scissor is in the surface's SAMPLES. Comparing them
unconverted is a no-op for every 2X tile -- max(1024 samples, 720 pixels) leaves
the band at 1024 -- so the collapsed tile never widened and the frame rendered
508 of its 720 rows. Multiplying dstBottom by the draw's own vertical sample
scale fixes it:

    frame non-black       70.4%  ->  99.1%   (off arm 97.9%)
    depth resolve         71.1%  ->  100.0%  (off arm 100.0%)
    colour resolves       70.4%  ->  99.2%   (off arm 99.0-99.5%)

The lesson worth keeping is the grep, not the arithmetic: `grep "pd.scissor"`
in the file that sets it found two sites and looked conclusive. The field is
written through a different expression in a different translation unit, and it
took a probe inside the reader to show that the reader was not the liar.

STILL OPEN under the model: resolve destination 0xcb91000 (the f25 pass) is 0%
non-zero with it on and 49.9% off.

### Note (2026-08-11)
retitle: the scissor legitimately changes between prepare and report

### Resolution (2026-08-11)
The table was not lying: CollapseEdramTiling widens a collapsed tile's scissor to the union of its resolve destinations, so 512 (the guest's band) becomes 720 (the union) between prepare and report. Found by probing pd.scissor from inside the table WRITER in the same run as the prepare-time probe, then grepping .scissor across the whole runtime rather than the one file that sets it. The same mixed unit was the blocker on the sample model, and converting dstBottom into samples took the MSAA arm from 70.4% to 99.1% of the frame non-black.

---
id: I024
kind: instrument
status: trusted
created: 2026-08-06
---

## Instrument

GEARS_REPLAY_DRAWS=<n> (tools/frame_replay): replay only the first n draws of a capture

## Validated by

DISTRUST IT ACROSS A TILE BOUNDARY -- established by its own control, not reasoned. Cutting bright.gfr at draw 462 (the end of tile 1) flips 49 draws from 'shaded' to 'rasterised_no_fragment' against the untruncated replay of the SAME draws, because this frame renders in two Xenos tiles (186 draws at window offset 0, 185 replayed at 0x7e000000) and the untile pass collapses the pair using draws from both halves. It reported 'our resolve 0 is 0 of 2,764,800 components non-zero' against the oracle's 18.4%, which is an artefact of the cut and NOT a divergence. Sound only when the cut falls on a boundary in the 'untile: N draw group(s)' line; the tool now warns by name.

## Known failure modes

(none recorded yet)

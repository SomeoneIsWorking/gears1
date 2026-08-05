---
id: I011
kind: instrument
status: trusted
created: 2026-08-05
---

## Instrument

GEARS_DRAW_RESOLVE_DUMP per-channel means + diag resolve_swap_rb

## Validated by

Positive: on courtyard.gfr the census separates two targets that the old merged range reported identically, giving R 0.076818/B 0.059357 for 0x311000 against R 0.059357/B 0.076818 for 0xc7f9000. Negative: target 0xcb91000 sums to zero green and prints 'NO RATIO: green sums to zero here' instead of a number, and depth targets print no channel line at all. resolve_swap_rb was cross-checked against the GEARS_DRAW_RESOLVE_NOSWAP control arm -- and the two DISAGREEING is what caught a wrong conclusion (catalog #62's retraction), so the column earns its place. tools/frame_hashes.sh unchanged across all 8 captures.

## Known failure modes

(none recorded yet)

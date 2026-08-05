---
id: I010
kind: instrument
status: trusted
created: 2026-08-05
---

## Instrument

tools/find_const_block.py

## Validated by

Run against BOTH classes before being trusted. POSITIVE: play_v2.gfr (the capture that renders black) contains the fatal c7||c8 pattern 7x. NEGATIVE: courtyard.gfr and act1_v2.gfr, which both render, do NOT contain it anywhere, while containing the working pattern 13x and 16x -- so the search is shown able to report absence and presence on the same corpus type. A missing file makes it print 'REFUSING to report ... Nothing was searched' and exit non-zero rather than report no matches. Guest addresses are only printed for hits inside the parsed block table; hits elsewhere print offsets and are called out as not being addresses.

## Known failure modes

(none recorded yet)

---
id: I023
kind: instrument
status: trusted
created: 2026-08-06
---

## Instrument

gfr_to_xtr.py --present resolve:N (render a named resolve of our own captured frame through the oracle)

## Validated by

Selftest arms cover both classes: resolve:0 on a 3-resolve capture presents the FIRST resolve (not the frame's last), a depth resolve is REFUSED by name, and an out-of-range index refuses while listing what does exist. On real data it produced a non-uniform picture (bright.gfr resolve 0: 20.7% non-zero, max 0.80, recognisable geometry) where --present frame produces uniform black -- so it discriminates. CAVEAT: it truncates playback, and a truncation inside a Xenos tile group is not a clean prefix on OUR side (see GEARS_REPLAY_DRAWS).

## Known failure modes

(none recorded yet)

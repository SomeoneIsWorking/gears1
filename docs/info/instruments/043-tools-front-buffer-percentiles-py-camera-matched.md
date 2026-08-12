---
id: I043
kind: instrument
status: trusted
created: 2026-08-12
---

## Instrument

tools/front_buffer_percentiles.py (camera-matched front-buffer distribution compare)

## Validated by

Decodes the console's raw guest bytes through layer_compare's own untile/unpack_dest (trusted I033) and cross-checks every geometry flag against the dump filename, REFUSING when a flag contradicts the name. A decode more than 1% non-finite is refused as a FAILED DECODE rather than reported as a difference. Reports median/p90/p99/p99.9/max/mean per channel because catalog #62 was misled twice by max and once by mean. Deliberately does NOT do layer_compare's pass-structure frame selection: on this data that picked console frame 873 over frame 571, the frame the capture was actually camera-gated to.

## Known failure modes

(none recorded yet)

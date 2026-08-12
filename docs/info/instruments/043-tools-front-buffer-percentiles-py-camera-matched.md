---
id: I043
kind: instrument
status: DISTRUSTED
created: 2026-08-12
distrusted_on: 2026-08-12
---

## Instrument

tools/front_buffer_percentiles.py (camera-matched front-buffer distribution compare)

## Validated by

Decodes the console's raw guest bytes through layer_compare's own untile/unpack_dest (trusted I033) and cross-checks every geometry flag against the dump filename, REFUSING when a flag contradicts the name. A decode more than 1% non-finite is refused as a FAILED DECODE rather than reported as a difference. Reports median/p90/p99/p99.9/max/mean per channel because catalog #62 was misled twice by max and once by mean. Deliberately does NOT do layer_compare's pass-structure frame selection: on this data that picked console frame 873 over frame 571, the frame the capture was actually camera-gated to.

## Known failure modes

(none recorded yet)

## DISTRUSTED 2026-08-12

It reported distributions for a pair that was never established to be the same picture, and I published two wrong conclusions from it. The camera gate matched the view-projection to a distance of 3.77 and that does NOT imply the same rendered scene: log-luminance correlation between the two sides is 0.07 (best 0.16 over flips and shifts up to +/-64px) where a pair that must agree scores 0.93 through the same metric. FIXED IN THE SAME COMMIT -- the tool now runs a same-picture gate before it prints anything and REFUSES below --min-corr 0.60, printing the positive control beside the score, and --selftest drives both classes (a dimmed 8-bit copy must pass; a spatially shuffled image with an IDENTICAL HISTOGRAM must fail). Re-trust it only for pairs that PASS that gate; every number it produced before this commit is void.

> Every result this instrument produced is suspect until it is re-validated.

## DISTRUSTED 2026-08-12

Superseded and its verdicts void: correlation-based pass scoring cannot report a CONSTANT buffer. A flat buffer has zero variance, numpy's corrcoef returns nan, and first_divergence.py printed that nan in the score column as though it were a number, poisoned the next row's delta, and named an innocent pass four rows downstream as the frame's frontier. The frame's most broken pass -- our second shadow mask, flat at 1.0 where the console puts 4.88% of the screen in shadow -- was the one it was structurally incapable of scoring. Any frontier or pass verdict produced before the degenerate check was added must be re-derived, not cited.

> Every result this instrument produced is suspect until it is re-validated.

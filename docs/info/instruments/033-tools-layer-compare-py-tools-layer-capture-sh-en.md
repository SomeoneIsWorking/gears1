---
id: I033
kind: instrument
status: trusted
created: 2026-08-11
---

## Instrument

tools/layer_compare.py + tools/layer_capture.sh, endian-correct and frame-selecting (supersedes I032)

## Validated by

Six self-test classes, each with the negative it could fail on, run by --selftest in a directory it empties first: (1) a tiling round trip at 4 and 8 bytes against the scalar ported address; (2) each 32-bit unpack against an arithmetic answer; (3) a k_24_8 depth pass that must MATCH beside a colour pass that must DIFFER; (4) the console FRAME chosen by pass STRUCTURE -- offered a one-pass frame matching byte for byte against a four-pass frame holding a real difference, it must take the four-pass one and keep the difference, and the choice line was separately shown to MOVE when a third frame is the better structural match; (5) two contiguous EDRAM bands that must rejoin and compare over the full height, beside a same-shaped buffer at an unrelated address that must NOT join; (6) the destination ENDIAN on a four-byte format -- the same pixel in guest order and byte-reversed must decode to the SAME colour, and under the wrong tag to a DIFFERENT one.

On real data: the aligned SP_Prison_P baseline is 16 of 16 passes shared with ZERO one-sided, the scene colour and scene depth MATCH over the full 720 rows, the three HDR resolves match, and both k_2_10_10_10 copies match. What it still reports as differing -- the two shadow-mask copies and the shadow atlas -- is catalog #91.

What it still cannot do: decode the three 352x182 k_16_16_16_16_FLOAT buffers (1.4% non-finite under every width and byte order, refused rather than shown), and it compares resolve DESTINATIONS only, so a pass consumed without a resolve is invisible to it.

## Known failure modes

(none recorded yet)

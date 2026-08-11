---
id: I032
kind: instrument
status: DISTRUSTED
created: 2026-08-11
distrusted_on: 2026-08-11
---

## Instrument

tools/layer_compare.py + tools/layer_capture.sh, depth-aware (supersedes I031): every executed copy of the paired gameplay frame is snapshotted, including DEPTH copies, and a depth pass is keyed the way the console keys it -- source from RB_DEPTH_INFO, destination format from RB_DEPTH_INFO.depth_format (22 k_24_8 / 23 k_24_8_FLOAT), not from RB_COPY_DEST_INFO.copy_dest_format

## Validated by

Three ways. (1) NEGATIVE CONTROL, run: --selftest now drives the real comparison over a synthetic depth pair and asserts it is joined, reported as not-value-compared, and counted; disabling the depth branch makes two of those four checks FAIL, so the check discriminates in both directions. (2) The two sides derive the depth key INDEPENDENTLY and agree: our runtime reads RB_DEPTH_INFO and names srcD000 f23 (scene) and srcD5A0 f22 (shadow maps); the console's fork derives the same two names through Xenia's own draw_util.cc GetResolveInfo. (3) On the paired capture the shared-pass count rose 9 -> 12, exactly the 3 depth passes, with no colour pass changing verdict. LIMIT, stated in its own output: depth VALUES are still not compared -- our destination holds decoded float depth and the console's holds packed guest bytes (catalog #35).

## Known failure modes

(none recorded yet)

## DISTRUSTED 2026-08-11

It decoded the CONSOLE's four-byte colour destinations with the dword's bytes in the wrong order: copy_dest_endian was applied to the eight-byte format and to depth and to nothing else, and every k_2_10_10_10 dump in this title is k8in32. That scrambles the bit fields rather than shifting a value -- the 2-bit alpha lands in the low bits and reads as RED, 96.9% of it zero -- and it produced catalog #95, a session of hypotheses about a renderer difference that did not exist. With the endian applied both copies MATCH. Superseded by I033, which is the same tool with the endian applied to every format, with a self-test that runs BOTH classes of the endian tag, plus console-frame selection by pass structure and the EDRAM band rejoin. Every number this instrument produced for a k_2_10_10_10 or k_8_8_8_8 pass before 2026-08-11 has to be re-read from a re-run.

> Every result this instrument produced is suspect until it is re-validated.

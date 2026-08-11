---
id: I032
kind: instrument
status: trusted
created: 2026-08-11
---

## Instrument

tools/layer_compare.py + tools/layer_capture.sh, depth-aware (supersedes I031): every executed copy of the paired gameplay frame is snapshotted, including DEPTH copies, and a depth pass is keyed the way the console keys it -- source from RB_DEPTH_INFO, destination format from RB_DEPTH_INFO.depth_format (22 k_24_8 / 23 k_24_8_FLOAT), not from RB_COPY_DEST_INFO.copy_dest_format

## Validated by

Three ways. (1) NEGATIVE CONTROL, run: --selftest now drives the real comparison over a synthetic depth pair and asserts it is joined, reported as not-value-compared, and counted; disabling the depth branch makes two of those four checks FAIL, so the check discriminates in both directions. (2) The two sides derive the depth key INDEPENDENTLY and agree: our runtime reads RB_DEPTH_INFO and names srcD000 f23 (scene) and srcD5A0 f22 (shadow maps); the console's fork derives the same two names through Xenia's own draw_util.cc GetResolveInfo. (3) On the paired capture the shared-pass count rose 9 -> 12, exactly the 3 depth passes, with no colour pass changing verdict. LIMIT, stated in its own output: depth VALUES are still not compared -- our destination holds decoded float depth and the console's holds packed guest bytes (catalog #35).

## Known failure modes

(none recorded yet)

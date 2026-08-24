---
id: C079
kind: claim
status: holds
created: 2026-08-24
tags: provisioning,gdf
depends: tools/gdf_extract.py, tests/test_gdf_extract.py
---

## Claim

The GDF extractor refuses malformed extents, traversal, cycles, path collisions, symlink escapes, short reads, and stale same-size resume output before publishing host files.

## Evidence

All 13 production-interface synthetic GDF tests passed inside the 62/62 parent CTest suite; tests include valid extraction and positive controls for each refusal class.

## What would falsify it

A constructed refusal case writes outside the destination, reuses byte-different output, publishes a short file, or any of the 13 controls stops producing its opposite expected result.

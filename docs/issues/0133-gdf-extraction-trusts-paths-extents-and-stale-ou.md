---
id: 133
title: GDF extraction trusts paths extents and stale outputs
status: resolved
symptom: disc extraction can escape its destination accept truncated extents or reuse stale same-size files
tags: provisioning,gdf,extractor,security
created: 2026-08-24
updated: 2026-08-24
---

## Root cause

The extractor treated disc directory entries as trusted host paths and trusted
their table and file extents. It also considered an existing same-size output a
successful resume without comparing its bytes. A malformed image could escape
the destination or cycle traversal, and a stale artifact from another disc could
silently become a build input.

## What was tried / dead ends

Normalizing joined path strings is insufficient because an existing symlink can
redirect a later component after the check. Size-only resume is likewise not an
integrity check.

## Resolution

### Resolution (2026-08-24)
Replaced path-string extraction with bounded volume/table/name/extent parsing, cycle and collision refusal, dirfd/O_NOFOLLOW destination confinement, exact short-read checks, byte-for-byte resume verification, and atomic replacement. All 13 synthetic positive and refusal tests pass.

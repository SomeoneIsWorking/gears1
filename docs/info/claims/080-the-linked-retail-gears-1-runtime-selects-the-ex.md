---
id: C080
kind: claim
status: holds
created: 2026-08-24
tags: title-profile,provisioning,runtime-image
depends: tools/title_identity.py#parse_xex_metadata, runtime/title_profile.cpp#ResolveTitleProfile, config/titles/gears1.toml
reconfirmed: 2026-08-24
verified_at: 2026-08-24 22:41:02
---

## Claim

The preserved Gears 1 identity contract names the exact XEX container and normalized image using both SHA-256 digests plus image base, size, and entry point. This is input-authentication evidence, not evidence that the missing x360port product executes.

## Evidence

The checked parser and independent hashing recorded container `df1041da...efe2d1`, normalized image `f61cc78e...ed5c`, base `0x82000000`, size `0xCE0000`, and entry `0x82612BF0`. Strict parser/profile tests reject every mutated identity field and partial parse publication.

## What would falsify it

An executable whose five-field identity differs from the exact profile is accepted by the future x360port/Gears adapter, or parser failure publishes a partial identity.

## Re-confirmed 2026-08-24

Historical execution details were removed with the generated product. They do not reconfirm runtime execution; only the exact identity values above remain live migration evidence.

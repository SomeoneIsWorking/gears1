---
id: I059
kind: instrument
status: trusted
created: 2026-08-22
---

## Instrument

test_ue3_host_lzo

## Validated by

Shows both answers: a synthetic LZO round trip and all three real Entry.xxx blocks succeed, while invalid arguments and a deliberately corrupted stream fail

## Known failure modes

The real-asset arm knows the fixed compression-stream offset in `Entry.xxx` and
checks total byte accounting plus the first logical name, not a full independent
payload digest or complete UE3 package load. It requires the externally
provisioned package at the configured `GEARS_GAME_DIR` and fails when that asset
is absent.

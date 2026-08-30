---
id: I067
kind: instrument
status: trusted
created: 2026-08-30
---

## Instrument

GEARS_NATIVE_RHI_OBSERVE actual NativeDrawInput materialization join

## Validated by

test_rhi_semantic_stream proves physical-alias matching, altered index-base and replay mismatch answers, zero-key refusal, mixed-outcome replay and inconsistent-source classification, both arrival orders, duplicate invalidation, and bounded one-sided expiry; current headless runs also produced matches, an explicit dropped-frame missing result, and both current-unmatched and no-current-unmatched reports

## Known failure modes

The source provenance identifies a ring or indirect-buffer span, not the guest function that wrote the packet. Writer ownership still requires title-call observation or a complete exact-revision packet-writer census.

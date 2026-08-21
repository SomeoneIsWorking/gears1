---
id: I057
kind: instrument
status: trusted
created: 2026-08-21
---

## Instrument

xenia-gpu-vulkan-trace-dump synchronous raw resolve capture

## Validated by

The no-flag chapter-45 run logged forced synchronous pipeline creation and emitted 27/27 raw destinations byte-identical to a separate explicit --vulkan_pipeline_creation_threads=0 control. Its required other-answer arm used GEARS_ORACLE_FORCE_DUMP_WHITE=1: all 27 destinations still emitted, the dump shader reported five forced-white pipelines, and C400 changed from SHA-256 d6b780f7... to b30b7be4.... This trusts raw resolve content on complete synchronous traces, not the separately distrusted intermediate checkpoint I051.

## Known failure modes

(none recorded yet)

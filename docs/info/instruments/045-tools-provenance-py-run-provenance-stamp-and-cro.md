---
id: I045
kind: instrument
status: trusted
created: 2026-08-12
---

## Instrument

tools/provenance.py (run-provenance stamp and cross-run pair check)

## Validated by

--selftest drives ALL THREE outcomes rather than only the passing one: MATCH between two dirs stamped with the same pair id and complementary roles (exit 0), MISMATCH between different pair ids (exit 2), and UNKNOWN for an unstamped dir (exit 3, explicitly NOT a pass). It also verifies the camera file is FROZEN into the capture directory and stays intact after the source file is overwritten -- the exact failure mode that caused C042. Wired into tools/layer_capture.sh (stamps both sides before the runs) and tools/front_buffer_percentiles.py (checks before measuring).

## Known failure modes

(none recorded yet)

---
id: I039
kind: instrument
status: trusted
created: 2026-08-12
---

## Instrument

GEARS_ORACLE_PRIM_STATS

## Validated by

Self-validating in both directions within a single run: of the six draws it measured, four returned large non-zero post-clip counts (5,254 to 5,412) and two returned zero, so it can produce both answers and a zero is a measurement rather than a silent failure. It also reports its denominators -- how many draws were measured and how many were dropped past the pool's capacity -- and refuses loudly, once, when the device lacks pipelineStatisticsQuery or hostQueryReset rather than reporting zero. Counts INPUT_ASSEMBLY_PRIMITIVES, CLIPPING_PRIMITIVES and FRAGMENT_SHADER_INVOCATIONS for every draw of one named vertex shader in the dumped frame. BLIND SPOT: a per-shader total over one frame only; it says nothing about WHICH primitives were lost, and fragment invocations are meaningless for these draws because the console gives them a null pixel shader.

## Known failure modes

(none recorded yet)

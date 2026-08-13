---
id: 105
title: skinned_frames stops its corpus scan at the first negative capture
status: resolved
symptom: `tools/skinned_frames.sh --list` claims to inspect every frame capture but prints only act1.gfr and no denominator
tags: tooling,diagnostic,character,capture
created: 2026-08-14
updated: 2026-08-14
---

Root cause: the script enables `set -e` and had two paths where the expected NONE class returned nonzero. First, it counted positives with `[[ $v == FOUND ]] && (( ++found ))`; the false test exited the script. After correcting that, list mode still ran the detector in a `pipefail` pipeline, and frame_replay's intentional exit 3 for NONE terminated the script after printing the first explanation. The selftest called `verdict()` twice directly and therefore exercised neither broken corpus reporting path. The fix uses an explicit `if` and a `listing()` helper that accepts only the documented 0/3 detector outcomes while requiring an explicit positive or negative line. The selftest now runs a mixed FOUND/NONE loop, requires scanned=2/found=1, and exercises both listing classes. Negative evidence before either fix: `--list` printed only `act1.gfr NONE` even though the captures glob contains many files. Positive evidence after the fix is the full named table plus its final `N of M` denominator.

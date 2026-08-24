---
id: 134
title: Fresh recompilation emits duplicate C++ labels that cached objects hid
status: resolved
symptom: A clean build of freshly regenerated ppc_recomp C++ fails with redefinition of loc_* labels even though generation reached 100%
tags: recompiler,cfg,generated-code,build,cache
created: 2026-08-24
updated: 2026-08-24
---

## Root cause


## What was tried / dead ends


## Resolution

### Resolution (2026-08-24)
Root cause: Function::Analyze can retain overlapping executable blocks for converging conditional paths, and Recompiler::Recompile iterated those raw blocks, emitting the overlap and its branch labels twice. Cached object files hid the defect until scratch/ppc was deleted and regenerated. The emitter now normalizes a copied Function block set before output; instruction_emission_test.cpp constructs overlapping blocks and proves loc_4008 is emitted once. A clean retail generation and all 191 generated translation units compile.

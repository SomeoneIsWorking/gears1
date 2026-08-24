---
id: 135
title: Fresh XenonRecomp output fails only after analysis because the output directory is absent
status: resolved
symptom: After cleaning scratch/ppc, XenonRecomp reaches output publication and aborts with Unable to open ppc_config.h
tags: recompiler,workflow,generated-code,clean-build
created: 2026-08-24
updated: 2026-08-24
---

## Root cause


## What was tried / dead ends


## Resolution

### Resolution (2026-08-24)
Root cause: SaveCurrentOutData assumed config.out_directory_path already existed, while the documented freshness step deliberately deletes scratch/ppc. The expensive analysis therefore succeeded before fopen exposed the missing directory. Recompiler::LoadConfig now creates and validates the configured output directory before loading/analyzing the executable; a clean retail regeneration after deleting scratch/ppc completed without a manual mkdir.

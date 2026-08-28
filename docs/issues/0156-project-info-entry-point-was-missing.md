---
id: 156
title: Project information entry point was missing
status: resolved
symptom: The required project information brief could only be run through the shared checkout because this project had no tools/info.py.
tags: tooling,registry,workflow
created: 2026-08-28
updated: 2026-08-28
---

## Root cause

The project-local resolver had never been added, even though the workflow requires
`info.py brief` before non-trivial work. Sessions therefore had to know the
shared checkout's machine-specific path, and a fresh checkout had no local
entry point to discover the canonical implementation.

## Resolution

Added `tools/info.py` as a thin resolver. It delegates to the canonical
`shared/re-harness/tools/info.py` through `RE_HARNESS_REPO`, `SHARED_DIR`, or
the conventional sibling checkout, and changes to the project root before
delegation so `docs/info` is always this project's registry. It refuses with
the paths checked when the shared authority is unavailable.

`project_info_entrypoint` invokes the resolver from the build directory in
CTest, covering the cwd failure mode.

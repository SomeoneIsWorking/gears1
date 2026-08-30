---
id: 158
title: Runtime boot log retains the revision from an older CMake configure
status: investigating
symptom: After a newer commit, ./run.sh builds with ninja reporting no work and gears1 logs an older Git revision
tags: build,launcher,diagnostic,cmake
created: 2026-08-30
updated: 2026-08-30
---

## Root cause\n\nruntime/CMakeLists.txt resolved Git HEAD only while CMake configured and embedded that value as a main.cpp compile definition. No build dependency represented later commits, so the build graph correctly considered main.cpp current while its diagnostic identity was stale.\n\n## Resolution\n\nIn progress: publish HEAD through a generated header before every gears1 target check, rewriting the header only when the revision changes.\n\n## Evidence\n\nPending post-commit launcher verification.

---
id: 177
title: Native engine does not decode terrain
status: open
symptom: levels built on terrain actors draw without their ground
tags: native-engine,terrain
state_items: S022
created: 2026-09-25
updated: 2026-09-25
---

Terrain actors and their components are neither counted by `scene::LevelScene`
nor decoded. Measure which Gears 1 levels place terrain before sizing this.

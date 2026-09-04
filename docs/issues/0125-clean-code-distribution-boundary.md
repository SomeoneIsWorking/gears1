---
id: 125
title: Clean-code distribution boundary
status: resolved
created: 2026-08-24
updated: 2026-09-04
state_items: S002,S015
tags: distribution,copyright,provenance
---

## Resolution

The repository accepts a user-owned image/archive, validates exact identity, and
keeps extracted files and runtime caches ignored. The tracked-tip distribution
gate rejects copyrighted game/cache artifacts, private engine dependencies,
binary payloads, and generated artifacts without reviewable provenance.

The separate migration-boundary gate scans tracked and untracked first-party
source, tools, configuration, and documentation so a derived guest-source product
or dormant selector cannot return outside the current compile graph.

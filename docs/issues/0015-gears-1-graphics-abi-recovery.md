---
id: 15
title: Gears 1 graphics ABI recovery
status: resolved
created: 2026-07-22
updated: 2026-09-04
state_items: S003,S012
tags: graphics,abi,re
---

## Result

The exact Gears 1 draw, shader, resolve, and presentation entry points plus the
shader-container and resource-layout facts needed by a title adapter are recorded
in `docs/d3d-seam.md`. The old runtime observation wrappers were deleted because
they depended on the retired CPU execution product.

These addresses are evidence only. They must be re-observed through the exact
authenticated Xenia image before `x360port` dispatch or a shared `x360ue3`
contract is authorized.

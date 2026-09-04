---
id: 141
title: Native RHI awaits live Xenia input
status: open
created: 2026-08-27
updated: 2026-09-04
state_items: S003,S012
tags: native-rhi,xenia,renderer
---

## Root cause

The native semantic stream, host resource registry, renderer-input projection,
and Vulkan resolve pieces are executor-independent, but their former live capture
bindings depended on the deleted CPU product. They currently build and pass
focused tests without receiving a real title frame.

## Required resolution

After `x360port` executes Gears 1, connect authenticated guest memory and device
callbacks to the existing bounded interfaces. Prove command ordering, resource
identity/lifetime, target and shader state, resolves, presentation, and pixels
against the oracle before bypassing guest graphics command construction.

---
id: 125
title: Public clean-code distribution retained proprietary-derived build paths and history
status: resolved
symptom: A public clone could depend on private UE3 source while deleted title caches, decoded shader replacements, and generated-code patchers remained in the repository or its history
tags: distribution,copyright,provenance,ue3,recompiler
created: 2026-08-22
updated: 2026-08-22
---

## Root cause

The product boundary treated a private source checkout, locally reconstructed title shaders, and post-processing of generated PPC as acceptable development inputs. That cannot produce a source-only public engine whose users provide only their own disc image.

## Resolution

The tracked tip is now a GearsUE3 recomp-plus-override engine: the private-source build island and reconstructed title shaders are removed; generated output is never patched; XenonRecomp emits retained bodies plus override-safe forwarders; and the clean-distribution gate rejects known private/game/cache paths, non-UTF-8 binaries, executable/archive magic, generated PPC, and SPIR-V without a tracked clean source.

The public `main` history was rewritten after explicit approval. Deleted title
caches, generated PPC/game artifacts, reconstructed title shaders, retired
private-source build paths, and their private-source path strings were removed;
purged-only commits were dropped. A fresh single-branch clone at
`bf2e829f0043237909b00b2b1722369b1cb5fb9a` passed both the tracked-tip and
full-history modes of `tools/check_distribution_clean.py`, and `origin/main`
resolved to that commit. Future commits and refs can reintroduce history
contamination, so the full-history gate remains a release requirement rather
than a one-time assertion.

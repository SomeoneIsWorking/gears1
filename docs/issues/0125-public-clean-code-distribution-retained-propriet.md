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

The current public history still contains deleted title caches and retired proprietary-derived files. The history gate refuses release until a separately approved rewrite removes them. Therefore resolved here means the root architecture and tracked tip are corrected, not that old public commits are clean.

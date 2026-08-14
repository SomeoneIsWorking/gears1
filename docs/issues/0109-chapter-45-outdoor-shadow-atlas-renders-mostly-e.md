---
id: 109
title: Chapter 45 outdoor shadow atlas renders mostly empty and underscaled
status: investigating
symptom: On the validated chapter-45 outdoor pair, the first 864x864 shadow-atlas depth resolve scores 0.463 against a drift-matched console self-yardstick of 1.000; native is 96.4% far depth while oracle is 87.8%, and native caster silhouettes are visibly much smaller/sparser
tags: render,oracle,shadow,depth,gameplay-scene
created: 2026-08-14
updated: 2026-08-14
---

Evidence: scratch/camerapair_chapter45_outdoor_wide_20260814, oracle winner f5737, strict static-world camera match 0.17 thresholds, UI longest-run shape 1/1, front buffer 0.8239 = 1.7 console frames of drift. Scene color 0.8254 vs self 0.8012 and scene depth 0.9579 vs self 0.9160 rule out a global viewpoint or base-pass failure. At native draw 2089 the first srcD5A0 f22 atlas is 96.36% far depth, mean 0.9830; oracle copy361 is 87.77% far, mean 0.9322. Visual decode shows four corresponding atlas quadrants but native caster silhouettes occupy much less of each tile. The following srcC2D0 f6 pass is also deficient (0.3610 vs self 0.6774). Native issues 8 D5A0 resolves while oracle issues 9, so ordinal alignment of later atlas copies is structurally unsafe, but this first full 864x864 copy is independently named and decodable. Previous Act-1 C051 scored its atlas above 0.99; this scene falsifies its universal expiry condition and proves coverage was scene-specific. Next discriminator: compare the four atlas draws shader/vertex/viewport/projection constants between guests at the winning frame; determine whether the smaller silhouettes come from unequal light matrices/title state or native viewport/clip transform before changing rendering.

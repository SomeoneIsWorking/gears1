---
id: 109
title: Chapter 45 outdoor shadow atlas renders mostly empty and underscaled
status: investigating
symptom: On the validated chapter-45 outdoor pair, the first 864x864 shadow-atlas depth resolve scores 0.463 against a drift-matched console self-yardstick of 1.000; native is 96.4% far depth while oracle is 87.8%, and native caster silhouettes are visibly much smaller/sparser
tags: render,oracle,shadow,depth,gameplay-scene
created: 2026-08-14
updated: 2026-08-14
---

Evidence: scratch/camerapair_chapter45_outdoor_wide_20260814, oracle winner f5737, strict static-world camera match 0.17 thresholds, UI longest-run shape 1/1, front buffer 0.8239 = 1.7 console frames of drift. Scene color 0.8254 vs self 0.8012 and scene depth 0.9579 vs self 0.9160 rule out a global viewpoint or base-pass failure. At native draw 2089 the first srcD5A0 f22 atlas is 96.36% far depth, mean 0.9830; oracle copy361 is 87.77% far, mean 0.9322. Visual decode shows four corresponding atlas quadrants but native caster silhouettes occupy much less of each tile. The following srcC2D0 f6 pass is also deficient (0.3610 vs self 0.6774). Native issues 8 D5A0 resolves while oracle issues 9, so ordinal alignment of later atlas copies is structurally unsafe, but this first full 864x864 copy is independently named and decodable. Previous Act-1 C051 scored its atlas above 0.99; this scene falsifies its universal expiry condition and proves coverage was scene-specific.

The four leading atlas draws are now joined by vertex count rather than ordinal. Three of them carry the same load-bearing c230..c237 light/projection rows on both guests (10,932 and 8,154 exactly; 5,814 differs by one float ULP in two translation components). Their assembled/survived-clip/fragment-invocation counts are respectively 3644/1829/59379 exactly on both sides, 2718/1373/16664 exactly, and 1938/1011/33281 oracle versus 1938/1011/33287 native. The 8,448-vertex draw has genuinely different c230..c241 and is a separate title-state/geometry divergence. Equal fragment invocations do NOT establish equal unique pixel coverage: the matched 10,932 tile stores 31,618 non-clear oracle pixels but only 13,491 native pixels.

The D24S8 resolve is ruled out as the source. `scratch/ch45_live_depth_valid_20260814` dumps native's live D32 depth after every matching shadow-depth draw. Its tile quantiles and means reproduce the first resolved atlas to 8-bit quantisation (for the 10,932 tile, live mean 0.96384335 and resolved mean 0.9638444). Therefore the loss exists while rasterising/testing the shadow draw, before the resolve shader reads or encodes depth. Next discriminator: capture/compare vertex-shader output positions or an equivalent per-primitive spatial distribution for the three matrix-matched draws, then distinguish wrong fetched/skinned vertices from wrong depth-test evolution. Aggregate pipeline statistics cannot make that distinction.

---
id: C057
kind: claim
status: holds
created: 2026-08-14
tags: oracle,render,pairing,character,skinning,translucency
depends: tools/camera_pair.sh, tools/menu_walk.sh, tools/ui_state_check.py, tools/first_divergence.py
---

## Claim

A continuous-movement camera pair reaches a clean, character-centered scene, and none of its 11 decodable resolve boundaries falls 0.15 below its own drift-matched oracle curve.

## Evidence

scratch/camerapair_forward_crossing_20260814 used `90:START~120 600:A 700:RX+ 780:RX0 820:LY+`, deliberately leaving movement active so native could cross the oracle position despite unequal wall-clock delta time. Oracle dumped 8/8 frames and selected f1121. Both roles carry pair id camerapair-20260813T220949Z-803526 and camera digest bca3ce33c0033cdd. Native logged one automatic storage selection; ui_state_check scanned 1649 draws and accepted clean class 2. The camera gate matched after 237 held frames at rotation 0.0038/0.005 and relative translation 0.00311/0.013, 0.76 thresholds. pair_score measured 0.9169 = 2.3 console frames. The rendered frame visibly centers a lit armored character and subtitle/translucent UI; the draw table includes the known skinned vertex shader 15cbc482459fe5b7. first_divergence at drift 2.3 compared 11 decodable resolves; maximum positive deficit 0.0136, none >=0.15. It separately refused one partial-row shadow-atlas buffer, two sparse passes, three non-finite f32 resolves and two console-only 1280x208 resolves.

## What would falsify it

A repeat failing startup/input/UI/provenance/camera/drift gates, evidence that the centered mesh is not a skinned character, or a corrected decoder/metric yielding a >=0.15 deficit on this preserved pair.

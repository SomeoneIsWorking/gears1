---
id: 178
title: Native engine renders only a headless overview frame
status: open
symptom: gears_level_render writes one PPM from an automatic three-quarter camera; there is no window, no player camera, and no frame loop
tags: native-engine,render,window
state_items: S022
created: 2026-09-25
updated: 2026-09-25
---

`render::LevelRenderer` draws into an offscreen target only. An interactive
product needs a swapchain presenter, a frame loop, and a camera driven by the
player's input, starting at the level's player start.

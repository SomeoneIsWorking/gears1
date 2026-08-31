---
id: 169
title: Launcher cannot provision 7z-wrapped disc images
status: resolved
symptom: user-owned Gears 2 and Gears 3 archives are not accepted by the shipping image resolver
state_items: S006,S008
tags: archive,provisioning,launcher,title-boundary
created: 2026-08-31
updated: 2026-08-31
---

The supported user inputs include direct XGD images, but the available Gears 2 and Gears 3 inputs are 7z archives containing one ISO each. The launcher must keep direct images unchanged while validating one safe archive member, enforcing archive bounds, requiring 7-Zip only for archive inputs, and materializing the image under ignored content-addressed storage.

### Resolution (2026-08-31)
Added a title-neutral 7z adapter that validates metadata, safe paths, entry and expanded-size bounds, exactly one unencrypted disc-image member, and content-addressed stable extraction. It is wired into prepare_title and CTest; focused Python tests and a real tiny 7z extraction pass, while direct image inputs remain unchanged.

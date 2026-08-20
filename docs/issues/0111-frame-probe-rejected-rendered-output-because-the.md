---
id: 111
title: Frame probe rejected rendered output because the readback buffer exposed MSAA allocation rows
status: resolved
symptom: The HTTP frame endpoint returns 503 even though renderer status says the frame rendered; RGBA byte count describes 1280x1440 while the public dimensions say 1280x720
tags: render,probe,readback,http
created: 2026-08-20
updated: 2026-08-20
---

## Root cause\n\nRenderer::RenderFrameImpl copied the full persistent MSAA-capacity readback allocation (W x SW sample rows) into GuestFramePixels while GuestFrameWidth/Height and every consumer contract describe the display extent (W x H). The probe correctly refused the inconsistent byte count.\n\n## Resolution and evidence\n\nCopy and publish exactly W x H x 4 RGBA bytes; the allocation may remain larger internally. A live Lucent-backed request then returned HTTP 200, a 2,764,816-byte P6 1280x720 PPM, and status reported guest frame 584 with 185 draws and 744,342 non-black pixels.

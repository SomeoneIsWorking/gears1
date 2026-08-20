---
id: C068
kind: claim
status: holds
created: 2026-08-20
tags: http,input,render
depends: runtime/debug_http.cpp#HandleRequest, runtime/input.cpp#SetRemotePad, runtime/graphics_probe.cpp#PublishGraphicsProbe, runtime/graphics_probe_render.cpp#RenderFrameWithGraphicsProbe, runtime/gpu_draw.cpp#Renderer::RenderFrameImpl
---

## Claim

The default loopback debug API can atomically drive guest XInput and obtain an authoritative 1280x720 renderer readback through Lucent

## Evidence

Live ./run.sh on port 32124: invalid button HTTP 400; A+START/ly=32767/rt=255 appeared as remote packet 1; release packet 2; disconnect source none; frame request HTTP 200 with 2,764,816-byte P6 and status guest frame 584, 185 draws, 744342 non-black pixels

## What would falsify it

A live default-launch route no longer reproduces those input transitions, response classes, or a correctly sized renderer PPM, or the input/render/HTTP ownership code changes

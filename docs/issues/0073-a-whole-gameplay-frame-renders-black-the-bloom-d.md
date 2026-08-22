---
id: 73
title: A whole gameplay frame renders black during the opening fade
status: resolved
symptom: the first captured gameplay frame is black although upstream scene colour contains content
tags: gpu,draw,post,black,frame,constants,oracle
created: 2026-08-05
updated: 2026-08-22
---

## Root cause

The capture selected the opening gameplay fade. The compatibility renderer had
valid upstream scene colour, but the final post chain intentionally produced a
black front buffer at that moment. A non-finite guest constant correlated with
these early black captures, but correlation did not prove a renderer defect.

## Resolution

A paired console capture showed the same final copies were black, so the frame
was faithful and the defect report was withdrawn. The capture workflow now
selects a later gameplay moment and refuses essentially black images when the
question requires visible-scene comparison.

Non-finite constant census remains a diagnostic only. It reports both positive
and negative denominators and does not classify a frame as wrong without a
same-moment oracle difference.

---
id: 52
title: A botched edit spliced timing into the middle of an if, and double-counted it
status: resolved
symptom: descriptor-update time in the frame breakdown is roughly twice what the work costs
tags: perf,measurement,instrument,gpu,draw
created: 2026-07-30
updated: 2026-07-30
---

runtime/gpu_draw.cpp had:

    if (!w.empty())
        const double descUpdateBegin = sinceStartMs();
    const double descUpdateBegin = sinceStartMs();
    vkUpdateDescriptorSets(...);
    msDescUpdate += sinceStartMs() - descUpdateBegin;
    msDescUpdate += sinceStartMs() - descUpdateBegin;

Three defects from one bad splice: the `if (!w.empty())` guard now guarded a dead declaration
so vkUpdateDescriptorSets ran unconditionally, and msDescUpdate was accumulated TWICE per
draw, so the reported descriptor-update cost was about double the real one. It compiled and
was committed, and every frame-cost breakdown taken after it carried the inflated figure.

Fixed to the obvious intent: begin, guarded call, one accumulate.

WHAT IT COST: the per-frame breakdown is the instrument used to decide what to optimise next,
so a wrong figure in it steers the work. See also #51 -- the same session found that the whole
draw-loop record region was declared and never accumulated at all, leaving 18 of 36 ms
unnamed.

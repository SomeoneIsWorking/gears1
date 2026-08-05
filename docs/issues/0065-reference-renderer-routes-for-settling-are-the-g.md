---
id: 65
title: Reference-renderer routes for settling 'are the graphics correct': what works and what does not
status: resolved
symptom: no ground truth for whether a rendered gameplay frame is correct; every colour or completeness question ends in the renderer's own judgement
tags: reference,ground-truth,method,colour,dead-end
created: 2026-08-05
updated: 2026-08-05
---

## The problem

Two sessions have now stalled on the same thing: a rendered frame looks plausible,
and there is nothing to check it against. Every conclusion becomes 'it looks right
to me', which has already produced one shipped-and-reverted red/blue swap (#62) and
one nearly-shipped present-path change (#64).

## What WORKS, cheapest first

1. **An image whose correct colours are known independently.** The Epic Games logo
   (orange) and the crimson-omen loading screen (red) settled the present-path
   question in minutes, where a graded gameplay scene could not have. USE THESE
   FIRST. Both appear without any input, early in a walk.
2. **Published in-engine stills, for palette only.** The 'Mad World' advert frame
   (Wikipedia, en:Gears_of_War_(video_game)) is real in-engine footage. Normalised
   channel means R 0.900 / G 1.087 / B 1.013 -- red is the LOWEST channel and the
   look is cold blue-grey, NOT the sepia the game's reputation suggests. Our frames
   have the same ordering. Good for 'is the cast plausible', useless for detail.
3. **Bit-exact A/B against the translated shader** (tools/verify_native_pass.sh).
   Settles arithmetic questions completely, and has retired the shader translator
   as a suspect (claim C002).

## What does NOT work: building Xenia as a reference

Attempted and abandoned deliberately, so nobody repeats it. Cloning xenia-canary
with submodules costs 3+ GB (DirectXShaderCompiler dominates and is Windows-only
here), and the build prerequisites ARE present on this machine (clang 22, gtk3,
x11, xcb, vulkan, sdl2 -- only libpulse missing). The build is not the blocker.

**The blocker is that a Xenia frame cannot be compared to ours.** Xenia has no
input scripting, so reaching the same scene needs a driven window; and even with
one, two runs of a dynamic scene never land on the same camera position or the same
frame, so no pixel comparison is possible -- only the same qualitative palette
judgement that item 2 already provides for free. Reaching Act 1 in THIS project
costs a 200-second scripted walk that only exists because we wrote it.

If a reference renderer is ever worth it, the shape that would actually pay is a
Xenia GPU TRACE of a scene plus its trace-dump tool, compared against a replay of
our own capture of the same trace -- i.e. a shared input, not a shared playthrough.
That is a much larger piece of work and it needs the trace formats bridged.

## The rule

Settle a colour or correctness question on an image whose correct appearance is
known independently. Never on a graded game scene, and never on 'this looks more
like how I remember the game'. In a desaturated image, swapping R and B turns
cyan-grey into sepia-grey and BOTH look like plausible Gears of War.

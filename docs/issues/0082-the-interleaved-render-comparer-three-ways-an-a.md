---
id: 82
title: The interleaved render comparer: three ways an A/B silently reports no difference
status: resolved
symptom: an in-process two-arm render comparison reports the arms identical when the knob demonstrably changes the frame
tags: tooling,render,ab,comparer,instrument
created: 2026-08-06
updated: 2026-08-06
---

`GEARS_DRAW_AB=<KNOB>` renders a captured frame twice in ONE process -- knob
unset, knob set -- and reports the first draw at which the two diverge. Getting
it to tell the truth took three fixes, and every one of them failed by reporting
"no divergence" rather than by breaking.

## 1. Persistent state carried arm A into arm B

The renderer keeps surfaces, pipelines, shader translations and caches between
frames. Almost every knob worth comparing is consumed while BUILDING that state
-- a surface's host format is chosen once, when the surface is created -- so arm
B rendered into arm A's surfaces and the knob changed nothing.

Fixed with `ResetRendererForComparison()` (`gpu_draw.h`), which waits for the
device and drops the persistent block so each arm is a clean render.

## 2. lucent caches config, so the knob never applied

`lucent::config` caches every environment read, and the only public way to drop
the caches is `set_prefix`, which early-returns when the prefix is unchanged. So
`setenv` between the arms had no effect at all: arm B read arm A's cached
answers.

STOPGAP, marked as one in `tools/frame_replay/main.cpp`: bounce the prefix to a
dummy and back to force the caches to clear. The proper fix is a
`lucent::config::refresh()` upstream.

## 3. The arms were written to /tmp

`std::filesystem::temp_directory_path()`. On this machine that is a tmpfs under a
per-user quota that logs and dumps exhaust, and filling it breaks every other run
on the box -- which is exactly how catalog #80's SIGBUS cascade started. Now
written under the run's own `GEARS_DRAW_DIR`.

## What makes it trustworthy

**It refuses rather than reports when the arms issue different draws.** The
tiling collapse changes the stream -- 550 rows against 724 -- and a row-by-row
comparison would then compare two different draws and report hundreds of
meaningless differences. That check fired on the tool's first real run and
invalidated an A/B that was about to be run by hand.

**Its no-difference message states what it cannot see**: "EITHER the knob changes
nothing about this frame, OR it never took effect". That message is what caught
defects 1 and 2 -- twice the tool told me not to trust its own negative, and
twice it was right.

**Validated against an independent method.** The in-process comparer and two
separate runs diffed by `tools/render_diff.py` give the same answer on
`GEARS_DRAW_FORCE_LDR`: first divergence at row 435, guest draw 612, arm A max
2.98/3.24/3.18 against 1.0/1.0/1.0.

### Note (2026-08-06)
## A FOURTH way it lied, and this one was a real crash (2026-08-06)

Running the comparer over eleven knobs, nine of them produced NO output at all.
They were not failing the comparison -- they were dumping core, and the driver
cannot report a crash because the crash takes the process with it.

    #9  gears::draw::ShaderCache::GetShader ... gpu_draw_shaders.cpp:61
    #8  std::filesystem::path::path(...)  <error: Cannot access memory at 0x31>

ROOT CAUSE, and it is the comparer's own doing:

    static const std::string& spvDir = lucent::config::text("DRAW_SPV_DUMP");

`lucent::config::text` returns a REFERENCE into lucent's config cache. The
comparer drops that cache deliberately between arms (the prefix bounce) so a
knob is re-read -- and every `static const std::string&` bound to it then
dangles. The next draw builds a `std::filesystem::path` from freed memory.

Two sites had it: `gpu_draw_shaders.cpp` and `gpu_draw_resolve_decode.cpp`. Both
now hold the string BY VALUE, which is correct regardless of what any tool does
to the cache, and both carry a note saying why.

This is a hazard the API invites: returning `const std::string&` makes
`static const auto&` look natural and it is a use-after-free waiting for anyone
who clears the cache. Worth a `refresh()` upstream that keeps entries alive, or
returning by value.

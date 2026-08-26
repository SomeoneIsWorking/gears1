#pragma once

// Renderer policy read once for a frame.
//
// Lucent configuration reads are thread-safe and cached, but every call still
// takes its mutex, constructs the prefixed key and searches the cache. None of
// these values may change while one frame is being recorded, so reading them
// from per-draw and per-texture code only adds contention to the hot path. A
// fresh snapshot per RenderFrame call also preserves frame_replay's ability to
// change a control arm between two renders in the same process.

#include <cstdint>

namespace gears::draw
{

struct FrameOptions
{
    bool applyDepthBias = true;
    bool applyTextureSigns = true;
    bool trackTextureDirtyPages = true;
    uint64_t textureBindingsPsHash = 0;
};

FrameOptions ReadFrameOptions();

} // namespace gears::draw

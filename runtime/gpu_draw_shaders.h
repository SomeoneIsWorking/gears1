#pragma once

// The shader translation cache: guest microcode -> a VkShaderModule the draw
// loop can bind, translated once per distinct key and kept for the run.
//
// The key is the TRIPLE (microcode hash, modification, clamp), never the hash
// alone. The modification carries the interpolator mask the vertex and pixel
// shaders exchange for a given draw, and the clamp is a property of the host
// target the draw writes -- so one microcode legitimately needs several
// modules across a frame, and collapsing the key would silently hand a draw
// the wrong one.
//
// This is also where a NATIVE PASS is substituted, and the only place: the
// translation still runs, so the rest of the frame keeps using the guest
// shader's binding layout, constant map and texture list.

#include <cstdint>
#include <map>
#include <tuple>

#include <vulkan/vulkan.h>

#include "gpu_draw.h"
#include "gpu_draw_renderer.h"
#include "gpu_draw_shader_override.h"
#include "gpu_draw_xlate.h"
#include "spirv_clamp.h"

namespace gears::draw
{

struct ShaderCache
{
    ShaderCache(Renderer& r, RendererPersistent& p)
        : R(r), P(p), xlate(p.xlate), modules(p.modules) {}

    // (microcode hash, modification, clamp). Clamp is 0 for none, 1 for RGBA,
    // 2 otherwise -- see GetShader for why it belongs in the key.
    using Key = std::tuple<uint64_t, uint64_t, int>;

    Renderer& R;
    RendererPersistent& P;
    std::map<Key, ShaderXlate>& xlate;
    std::map<Key, VkShaderModule>& modules;
    ShaderOverride diagnosticOverride;

    // What TRANSLATION cost this run, accumulated inside GetShader so the
    // number covers the misses and not the lookups. The frame timing line
    // reports it inside the shader-lookup figure, so it has to be measured
    // where it is actually spent.
    double msTranslate = 0;

    // Translates and builds on a miss, returns the cached pair on a hit.
    // False means the microcode could not be translated or the module could
    // not be created -- the caller must skip the draw rather than bind
    // whatever was there before.
    bool GetShader(bool isVertex, const uint8_t* uc, size_t sz, uint64_t hash,
                   uint64_t modification, bool clampOutput, ClampMode clampMode,
                   ShaderXlate*& outX, VkShaderModule& outM);
};

} // namespace gears::draw

// The shader translation cache. gpu_draw_shaders.h says what the cache is
// keyed on and why; this is the miss path -- translate, dump if asked,
// substitute a native pass, clamp, create the module.

#include "gpu_draw_shaders.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <lucent/config.h>
#include <lucent/log.h>

#include "native_pass.h"

namespace gears::draw
{

using Clock = std::chrono::steady_clock;

bool ShaderCache::GetShader(bool isVertex, const uint8_t *uc, size_t sz, uint64_t hash,
                            uint64_t modification, bool clampOutput, ClampMode clampMode,
                            ShaderXlate *&outX, VkShaderModule &outM)
{
    diagnosticOverride.Observe(isVertex, hash, modification);
    const Key key{hash, modification, clampOutput ? (clampMode == ClampMode::kRgba ? 1 : 2) : 0};
    auto xit = xlate.find(key);
    if (xit == xlate.end())
    {
        ShaderXlate x;
        const auto t0 = Clock::now();
        const bool translated = TranslateShader(isVertex, uc, sz, hash, modification, x);
        msTranslate += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        if (!translated)
            return false;
        xit = xlate.emplace(key, std::move(x)).first;
        // GEARS_DRAW_SPV_DUMP=<dir>: the TRANSLATED module, as the runtime
        // actually built it for THIS draw's modification key.
        //
        // WHY IT HAD TO EXIST. Writing a native pass means implementing the
        // translator's interface exactly -- descriptor bindings, uniform block
        // sizes, interpolator locations -- and getting any of it wrong is not
        // a validation error, it samples a different image and still draws a
        // plausible picture (docs/native-renderer.md). The only modules on
        // disk were the offline ones in scratch/shaders/bound_out/, translated
        // with NO modification key, so they carry no interpolator inputs and a
        // colour write mask of zero. This writes the real one.
        //
        // Always the TRANSLATED module, never the native substitute: the
        // reference is the thing being matched, and dumping our own output
        // instead would let a native pass "verify" against itself.
        {
            // BY VALUE, not by reference. lucent::config::text returns a
            // reference into its own cache, and that cache can be dropped --
            // the interleaved comparer drops it deliberately between arms to
            // make a knob re-read. A `static const std::string&` then dangles
            // and the next draw crashes inside std::filesystem::path.
            static const std::string spvDir{lucent::config::text("DRAW_SPV_DUMP")};
            if (!spvDir.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(spvDir, ec);
                char name[128];
                std::snprintf(name, sizeof name, "%s_%016llx_mod%016llx.spv",
                              isVertex ? "vs" : "ps", static_cast<unsigned long long>(hash),
                              static_cast<unsigned long long>(modification));
                const std::filesystem::path out = std::filesystem::path(spvDir) / name;
                std::ofstream f(out, std::ios::binary);
                if (f)
                {
                    f.write(reinterpret_cast<const char *>(xit->second.spirv.data()),
                            std::streamsize(xit->second.spirv.size()));
                    lucent::debug("draw", "translated module -> {} ({} bytes)", out.string(),
                                  xit->second.spirv.size());
                }
                else
                {
                    // A dump that silently writes nothing is worse than none:
                    // the next reader concludes the shader was never bound.
                    lucent::error("draw", "GEARS_DRAW_SPV_DUMP: cannot write {}", out.string());
                }
            }
        }
        // A NATIVE PASS STANDS IN HERE, and only here: the translation still
        // ran, so its binding layout, constant map and texture list are what
        // the rest of the frame uses -- the native module implements the same
        // interface with our own arithmetic. Substituting at the module keeps
        // every other property of the draw the guest's.
        const gears::native::Pass *nat = isVertex ? nullptr : gears::native::Find(hash);
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        const std::vector<uint32_t> *overrideCode =
            diagnosticOverride.CodeFor(isVertex, hash, modification);
        if (overrideCode != nullptr)
        {
            mi.codeSize = overrideCode->size() * sizeof(uint32_t);
            mi.pCode = overrideCode->data();
            lucent::warn("draw",
                         "DIAGNOSTIC SUBSTITUTION: pixel shader {:#018x}"
                         " is using GEARS_DRAW_PS_OVERRIDE_SPV, not the title's"
                         " translated module",
                         hash);
        }
        else if (nat != nullptr)
        {
            mi.codeSize = nat->spirv.size() * sizeof(uint32_t);
            mi.pCode = nat->spirv.data();
            lucent::info("native",
                         "pass \"{}\" is rendering natively for pixel"
                         " shader {:#018x} ({} SPIR-V words, from {})",
                         nat->name, hash, nat->spirv.size(), nat->evidence);
        }
        else
        {
            mi.codeSize = xit->second.spirv.size();
            mi.pCode = reinterpret_cast<const uint32_t *>(xit->second.spirv.data());
        }
        // THE CLAMP THE HOST RENDER TARGET NO LONGER PERFORMS. Applied to the
        // module, not the draw, so it is cached with it.
        // It applies to a NATIVE module too. Exempting native passes would
        // make them diverge from the translated shader on exactly the draws
        // this fixes -- and the A/B gate would then compare two different
        // things and call the difference a native-pass bug.
        std::vector<uint32_t> clampedCode;
        if (clampOutput)
        {
            clampedCode.assign(static_cast<const uint32_t *>(mi.pCode),
                               static_cast<const uint32_t *>(mi.pCode) +
                                   mi.codeSize / sizeof(uint32_t));
            if (ClampFragmentOutputs(clampedCode, clampMode))
            {
                mi.codeSize = clampedCode.size() * sizeof(uint32_t);
                mi.pCode = clampedCode.data();
            }
            else
                // Refusal is not silent: the draw renders unclamped, which is
                // the defect this exists to fix, so it has to be visible.
                lucent::warn("draw",
                             "pixel shader {:#018x} could not be clamped"
                             " to its fixed-point render target's range; this draw blends"
                             " as if the target were HDR",
                             hash);
        }
        VkShaderModule m = VK_NULL_HANDLE;
        if (vkCreateShaderModule(R.device, &mi, nullptr, &m) != VK_SUCCESS)
            return false;
        modules[key] = m;
    }
    outX = &xit->second;
    outM = modules[key];
    return true;
}

} // namespace gears::draw

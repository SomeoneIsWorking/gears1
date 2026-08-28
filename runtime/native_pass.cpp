#include "native_pass.h"

#include <lucent/config.h>
#include <lucent/log.h>

#include "native_scene_composite_spv.h"

namespace gears::native
{

namespace
{

gears::draw::ShaderInterface SceneCompositeInterface()
{
    gears::draw::ShaderInterface interface;
    interface.ok = true;
    interface.floatBitmap[0] = 0x3;
    interface.floatBitmap[3] = 1ull << 63;
    interface.floatCount = 3;
    interface.textures = {{0, 1}, {0, 1}};
    interface.samplers = {{0, 3, 3, 3, 7}};
    interface.requiredInterpolatorMask = 0x3;
    return interface;
}

// Hashes are factual interoperability metadata observed from a user-provided
// image. No title-derived shader implementation is distributed. An empty module
// is a declaration only, and Find() refuses it.
const std::vector<Pass> &RosterStorage()
{
    static const std::vector<Pass> roster = {
        Pass{0xea0007942db096adull,
             "movie YUV composite",
             "observed pass identity; clean implementation not yet provided",
             {}},
        Pass{0x501ac5d8692bf7b6ull, "full-screen scene composite",
             "observed full-screen contract; post-swizzle signs are read from"
             " the translated system-constant field and view routing is host-owned",
             NativeSceneCompositeSpirv(), SceneCompositeInterface()},
        Pass{0x9610bf8038af9aafull,
             "post-process blend",
             "observed pass identity; clean implementation not yet provided",
             {}},
        Pass{0x1f1a3f779667a02aull,
             "base pass, directional-lightmap material",
             "observed pass identity; clean implementation not yet provided",
             {}},
        Pass{0xd99a15450a08043aull,
             "base pass, directional-lightmap + specular exponent",
             "observed pass identity; clean implementation not yet provided",
             {}},
        Pass{0xffdafff8542ddcd6ull,
             "base pass, directional-lightmap + blended diffuse",
             "observed pass identity; clean implementation not yet provided",
             {}},
    };
    return roster;
}

} // namespace

const std::vector<Pass> &Roster()
{
    return RosterStorage();
}

bool Enabled()
{
    static const bool on = lucent::config::flag("NATIVE_PASSES");
    return on;
}

const Pass *Find(uint64_t pixelShaderHash)
{
    if (!Enabled() || pixelShaderHash == 0)
        return nullptr;
    for (const Pass &p : RosterStorage())
        if (p.pixelShaderHash == pixelShaderHash && !p.spirv.empty())
            return &p;
    return nullptr;
}

void ReportRoster()
{
    size_t implemented = 0, declared = 0;
    for (const Pass &p : RosterStorage())
        (p.spirv.empty() ? declared : implemented)++;

    if (!Enabled())
    {
        lucent::info("native",
                     "native passes OFF ({} implemented, {} declared)."
                     " Every draw uses the title's translated shader, as before"
                     " (GEARS_NATIVE_PASSES=1 switches substitution on)",
                     implemented, declared);
        return;
    }
    // ON with nothing implemented is the state that must not read as success.
    lucent::Line line;
    line.add("native passes ON: {} implemented, {} declared but NOT written --"
             " a declared pass renders through the title's translated shader"
             " exactly as if this were off. Roster:",
             implemented, declared);
    for (const Pass &p : RosterStorage())
        line.add(" [{} {}{}]", p.name, p.spirv.empty() ? "declared" : "native",
                 p.pixelShaderHash != 0 ? "" : ", hash unknown");
    line.flush(lucent::Level::Info, "native");
    if (implemented == 0)
        lucent::warn("native",
                     "GEARS_NATIVE_PASSES=1 changes NOTHING in this build:"
                     " no pass in the roster has a module yet. See docs/native-renderer.md");
}

} // namespace gears::native

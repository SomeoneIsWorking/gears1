#include "native_pass.h"

#include <lucent/config.h>
#include <lucent/log.h>

namespace gears::native
{

namespace
{

// THE ROSTER. One entry per UE3 pass this renderer intends to own, in the order
// docs/native-renderer.md gives: fog first because it is a single full-screen pass
// whose maths fits on a page and can be compared pixel for pixel, then the base
// pass, which is where the frame's content is.
//
// The hashes are the title's own, taken from the shader census on a captured
// gameplay frame (`GEARS_DRAW_FRAME_LIST=1` reports the bound pair per draw).
// 0x501ac5d8692bf7b6 is the full-screen pass that samples the resolved scene
// target -- the hot pair this project has instrumented since the first draw, and
// the pass every frame ends on.
//
// An entry with no module is a DECLARATION: it says this pass is claimed and not
// yet written, and Find() refuses it. That distinction is the whole reason the
// roster is a table rather than a set of ifs -- "native passes are on" and "native
// passes are on and none exist" have to read differently in the log.
const std::vector<Pass>& RosterStorage()
{
    static const std::vector<Pass> roster = {
        Pass{0x501ac5d8692bf7b6ull, "full-screen scene composite",
             "Engine/Shaders/PostProcessCommon.usf + MaterialTemplate.usf", {}},
        Pass{0ull, "height fog",
             "Engine/Shaders/HeightFogCommon.usf, Src/Engine/Src/FogRendering.cpp", {}},
        Pass{0ull, "base pass",
             "Engine/Shaders/BasePassPixelShader.usf, Src/Engine/Src/BasePassRendering.cpp", {}},
    };
    return roster;
}

} // namespace

const std::vector<Pass>& Roster() { return RosterStorage(); }

bool Enabled()
{
    static const bool on = lucent::config::flag("NATIVE_PASSES");
    return on;
}

const Pass* Find(uint64_t pixelShaderHash)
{
    if (!Enabled() || pixelShaderHash == 0)
        return nullptr;
    for (const Pass& p : RosterStorage())
        if (p.pixelShaderHash == pixelShaderHash && !p.spirv.empty())
            return &p;
    return nullptr;
}

void ReportRoster()
{
    size_t implemented = 0, declared = 0;
    for (const Pass& p : RosterStorage())
        (p.spirv.empty() ? declared : implemented)++;

    if (!Enabled())
    {
        lucent::info("native", "native passes OFF ({} implemented, {} declared)."
            " Every draw uses the title's translated shader, as before"
            " (GEARS_NATIVE_PASSES=1 switches substitution on)",
            implemented, declared);
        return;
    }
    // ON with nothing implemented is the state that must not read as success.
    lucent::Line line;
    line.add("native passes ON: {} implemented, {} declared but NOT written --"
             " a declared pass renders through the title's translated shader"
             " exactly as if this were off. Roster:", implemented, declared);
    for (const Pass& p : RosterStorage())
        line.add(" [{} {}{}]", p.name, p.spirv.empty() ? "declared" : "native",
                 p.pixelShaderHash != 0 ? "" : ", hash unknown");
    line.flush(lucent::Level::Info, "native");
    if (implemented == 0)
        lucent::warn("native", "GEARS_NATIVE_PASSES=1 changes NOTHING in this build:"
            " no pass in the roster has a module yet. See docs/native-renderer.md");
}

} // namespace gears::native

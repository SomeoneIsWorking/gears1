#include "native_pass.h"

#include <lucent/config.h>
#include <lucent/log.h>

namespace gears::native
{

const std::vector<Pass> &Roster()
{
    return ExactTitleRoster();
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
    for (const Pass &p : Roster())
        if (p.pixelShaderHash == pixelShaderHash && !p.spirv.empty())
            return &p;
    return nullptr;
}

const Pass *FindVertex(uint64_t vertexShaderHash)
{
    if (!Enabled() || vertexShaderHash == 0)
        return nullptr;
    for (const Pass &p : Roster())
        if (p.vertexShaderHash == vertexShaderHash && !p.vertexSpirv.empty())
            return &p;
    return nullptr;
}

void ReportRoster()
{
    size_t implemented = 0, declared = 0;
    for (const Pass &p : Roster())
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
    for (const Pass &p : Roster())
        line.add(" [{} {}{}]", p.name, p.spirv.empty() ? "declared" : "native",
                 p.pixelShaderHash != 0 ? "" : ", hash unknown");
    line.flush(lucent::Level::Info, "native");
    if (implemented == 0)
        lucent::warn("native",
                     "GEARS_NATIVE_PASSES=1 changes NOTHING in this build:"
                     " no pass in the roster has a module yet. See docs/native-renderer.md");
}

} // namespace gears::native

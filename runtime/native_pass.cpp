#include "native_pass.h"
#include "native_pass_basepass_spv.h"
#include "native_pass_gamma_spv.h"
#include "native_pass_movie_spv.h"
#include "native_pass_uber_spv.h"

#include <lucent/config.h>
#include <lucent/log.h>

namespace gears::native
{

namespace
{

// THE ROSTER. One entry per UE3 pass this renderer intends to own. The order it
// used to give -- fog, then the base pass -- came from UE3's source rather than
// from this title's frames, and the fog pass turned out not to exist here at all
// (see the withdrawal below). Which draws are which UE3 pass is now a measurement:
// tools/pass_structure.py over a GEARS_DRAW_DIAG table.
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
        // IMPLEMENTED. The startup movie's YUV->RGB composite, written from the
        // title's own 33-dword microcode (runtime/shaders/movie_yuv.frag) rather
        // than translated from it. The matrix and offsets are still the guest's,
        // read from its float-constant UBO -- a native pass reproduces the
        // OPERATION, not the numbers.
        Pass{0xea0007942db096adull, "movie YUV composite",
             "the title's ps_ea000794 microcode; layout per gpu_draw_xlate",
             MovieYuvSpirv()},
        // IMPLEMENTED. The pass every frame ends on: scene colour, modulated,
        // gamma'd and exposed (runtime/shaders/scene_gamma.frag).
        Pass{0x501ac5d8692bf7b6ull, "full-screen scene composite",
             "the title's ps_501ac5d8 microcode; UE3 Engine/Shaders/PostProcessCommon.usf",
             SceneGammaSpirv()},
        // IMPLEMENTED. UE3's uber post-process blend: the depth-of-field
        // composite against a blurred copy of the scene, then the scene colour
        // transform (shadows, highlights, midtones, desaturation), then the
        // output gamma. The last pass to touch the frame's colour before motion
        // blur -- runtime/shaders/uber_post_blend.frag.
        Pass{0x9610bf8038af9aafull, "uber post-process blend (DOF + colour transform)",
             "the title's ps_9610bf8038af9aaf microcode; UE3 CalcUnfocusedPercent"
             " and the scene colour transform",
             UberPostBlendSpirv()},
        // WITHDRAWN: height fog. UE3's RenderFog (FogRendering.cpp:614) is the
        // only pass in the engine that draws with colour mask RGB and
        // BF_One/BF_SourceAlpha blending, and NO draw in any capture this
        // project has matches it -- 2,558 draws across four captures, four
        // candidates by colour mask, all four the motion-blur shader
        // 0x629226076307234e with blending off. Declaring it kept a pass on
        // this roster that the title does not run. Claim C004, catalog #71.
        // IMPLEMENTED. The first native pass that draws the WORLD rather than a
        // full-screen composite: UE3's base pass for a directional-lightmap
        // material -- runtime/shaders/base_pass_lightmap.frag. One of the 44
        // base-pass materials in an Act 1 frame, and the hottest of them.
        Pass{0x1f1a3f779667a02aull, "base pass, directional-lightmap material",
             "the title's ps_1f1a3f779667a02a microcode; UE3"
             " Engine/Shaders/BasePassPixelShader.usf and BasePassCommon.usf",
             BasePassLightmapSpirv()},
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

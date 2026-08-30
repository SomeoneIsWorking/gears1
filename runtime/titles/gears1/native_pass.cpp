#include "native_pass.h"

#include "native_scene_composite_spv.h"
#include "native_scene_composite_vertex_spv.h"

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

gears::draw::ShaderInterface SceneCompositeVertexInterface()
{
    gears::draw::ShaderInterface interface;
    interface.ok = true;
    interface.floatBitmap[0] = 0xFull;
    interface.floatCount = 4;
    interface.vertexBindings = {{95, 12}};
    interface.requiredInterpolatorMask = 0x3;
    return interface;
}

} // namespace

const std::vector<Pass> &ExactTitleRoster()
{
    // Hashes are factual interoperability metadata observed from the exact
    // user-provided Gears 1 image. No title-derived shader implementation is
    // distributed. An empty module is a declaration only, and Find() refuses it.
    static const std::vector<Pass> roster = {
        Pass{0xea0007942db096adull,
             "movie YUV composite",
             "observed pass identity; clean implementation not yet provided",
             {}},
        Pass{0x501ac5d8692bf7b6ull, "full-screen scene composite",
             "observed full-screen contract; post-swizzle signs are read from"
             " the translated system-constant field and view routing is host-owned",
             NativeSceneCompositeSpirv(), SceneCompositeInterface(), 0x5363d0746b3ef666ull,
             NativeSceneCompositeVertexSpirv(), SceneCompositeVertexInterface()},
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

} // namespace gears::native

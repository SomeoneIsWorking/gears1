#include "native_pass.h"

// Exact Gears 1 roster contract. Generic enable/find behavior is exercised
// through the shipping implementation linked beside the exact adapter.

#include <algorithm>
#include <cassert>
#include <cstdint>

namespace
{

constexpr std::uint64_t kSceneComposite = 0x501ac5d8692bf7b6ull;
constexpr std::uint64_t kSceneCompositeVertex = 0x5363d0746b3ef666ull;
constexpr std::uint32_t kSpirvMagic = 0x07230203u;

void TestSceneCompositeModuleIsImplemented()
{
    const auto &roster = gears::native::Roster();
    assert(&roster == &gears::native::ExactTitleRoster());
    const auto scene =
        std::find_if(roster.begin(), roster.end(), [](const gears::native::Pass &pass)
                     { return pass.pixelShaderHash == kSceneComposite; });
    assert(scene != roster.end());
    assert(scene->spirv.size() >= 5);
    assert(scene->spirv.front() == kSpirvMagic);
    assert(scene->spirv[3] != 0);
    assert(scene->name != nullptr);
    assert(scene->evidence != nullptr);
    assert(scene->shaderInterface.ok);
    assert(scene->shaderInterface.floatBitmap[0] == 0x3);
    assert(scene->shaderInterface.floatBitmap[3] == (1ull << 63));
    assert(scene->shaderInterface.floatCount == 3);
    assert(!scene->shaderInterface.floatDynamicAddressing);
    assert(scene->shaderInterface.textures.size() == 2);
    for (const auto &texture : scene->shaderInterface.textures)
    {
        assert(texture.fetchConstant == 0);
        assert(texture.dimension == 1);
    }
    assert(scene->shaderInterface.samplers.size() == 1);
    const auto &sampler = scene->shaderInterface.samplers.front();
    assert(sampler.fetchConstant == 0);
    assert(sampler.magFilter == 3);
    assert(sampler.minFilter == 3);
    assert(sampler.mipFilter == 3);
    assert(sampler.anisoFilter == 7);
    assert(scene->shaderInterface.requiredInterpolatorMask == 0x3);
    assert(scene->vertexShaderHash == kSceneCompositeVertex);
    assert(scene->vertexSpirv.size() >= 5);
    assert(scene->vertexSpirv.front() == kSpirvMagic);
    assert(scene->vertexShaderInterface.ok);
    assert(scene->vertexShaderInterface.floatBitmap[0] == 0xFull);
    assert(scene->vertexShaderInterface.floatCount == 4);
    assert(scene->vertexShaderInterface.vertexBindings.size() == 1);
    assert(scene->vertexShaderInterface.vertexBindings.front().fetchConstant == 95);
    assert(scene->vertexShaderInterface.vertexBindings.front().strideWords == 12);
    assert(scene->vertexShaderInterface.requiredInterpolatorMask == 0x3);
    if (gears::native::Enabled())
    {
        assert(gears::native::Find(kSceneComposite) == &*scene);
        assert(gears::native::FindVertex(kSceneCompositeVertex) != nullptr);
    }
    else
    {
        assert(gears::native::Find(kSceneComposite) == nullptr);
        assert(gears::native::FindVertex(kSceneCompositeVertex) == nullptr);
    }
}

void TestUnimplementedDeclarationsRemainRefused()
{
    const auto &roster = gears::native::Roster();
    const auto movie =
        std::find_if(roster.begin(), roster.end(), [](const gears::native::Pass &pass)
                     { return pass.pixelShaderHash == 0xea0007942db096adull; });
    assert(movie != roster.end());
    assert(movie->spirv.empty());
    assert(gears::native::Find(movie->pixelShaderHash) == nullptr);
    assert(gears::native::Find(0) == nullptr);
    assert(gears::native::FindVertex(0) == nullptr);
}

} // namespace

int main()
{
    TestSceneCompositeModuleIsImplemented();
    TestUnimplementedDeclarationsRemainRefused();
    return 0;
}

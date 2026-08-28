#include "native_pass.h"

#include <algorithm>
#include <cassert>
#include <cstdint>

namespace
{

constexpr std::uint64_t kSceneComposite = 0x501ac5d8692bf7b6ull;
constexpr std::uint32_t kSpirvMagic = 0x07230203u;

void TestSceneCompositeModuleIsImplemented()
{
    const auto &roster = gears::native::Roster();
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
}

void TestUnimplementedDeclarationsRemainRefused()
{
    const auto &roster = gears::native::Roster();
    const auto movie =
        std::find_if(roster.begin(), roster.end(), [](const gears::native::Pass &pass)
                     { return pass.pixelShaderHash == 0xea0007942db096adull; });
    assert(movie != roster.end());
    assert(movie->spirv.empty());
}

} // namespace

int main()
{
    TestSceneCompositeModuleIsImplemented();
    TestUnimplementedDeclarationsRemainRefused();
    return 0;
}

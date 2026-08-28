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

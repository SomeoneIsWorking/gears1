#include <cstdio>
#include <vector>

#include "gpu_draw_untile.h"

namespace
{

int g_failures = 0;

void Check(bool condition, const char *message)
{
    if (condition)
        return;
    std::printf("FAIL: %s\n", message);
    ++g_failures;
}

gears::draw::PreparedDraw Geometry(uint32_t windowOffset)
{
    gears::draw::PreparedDraw draw{};
    draw.surfaceBase = 0x400;
    draw.windowOffset = windowOffset;
    draw.count = 6;
    draw.indexed = true;
    draw.primType = 4;
    draw.vsHash = 0x1111;
    draw.psHash = 0x2222;
    draw.colorMask = 0xF;
    draw.depthControl = 0x12;
    draw.blend0 = 0x1010101;
    draw.scissor.extent = {1280, 100};
    return draw;
}

gears::draw::PreparedDraw Resolve(int32_t destinationY)
{
    gears::draw::PreparedDraw draw{};
    draw.isResolve = true;
    draw.resolveDest = 0x100000;
    draw.resolveDstY = destinationY;
    draw.resolveSrcRect.extent = {1280, 100};
    return draw;
}

void TestDiagnosticsDoNotChangeTransformation()
{
    const std::vector<gears::draw::PreparedDraw> tiled = {Geometry(0), Resolve(0),
                                                          Geometry(0xFF9C0000), Resolve(100)};
    auto reported = tiled;
    auto silent = tiled;
    uint32_t reportedIssued = 2;
    uint32_t silentIssued = 2;

    gears::draw::CollapseEdramTiling(reported, reportedIssued, false, true);
    gears::draw::CollapseEdramTiling(silent, silentIssued, false, false);

    Check(reportedIssued == 1 && silentIssued == 1,
          "both policies collapse the replayed geometry draw");
    Check(reported.size() == 2 && silent.size() == 2,
          "both policies retain only the base draw and its resolve");
    Check(reported[0].scissor.extent.height == 200 && silent[0].scissor.extent.height == 200,
          "both policies widen the base tile scissor to the full destination");
    Check(reported[1].resolveSrcRect.extent.height == 200 &&
              silent[1].resolveSrcRect.extent.height == 200,
          "both policies widen the base resolve to the full destination");
}

} // namespace

int main()
{
    TestDiagnosticsDoNotChangeTransformation();
    if (g_failures == 0)
    {
        std::printf("all gpu draw untile tests passed\n");
        return 0;
    }
    std::printf("%d gpu draw untile test(s) FAILED\n", g_failures);
    return 1;
}

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>

#include "gpu_draw.h"
#include "gpu_draw_native_input.h"

namespace
{

constexpr bool Near(float actual, float expected)
{
    return std::fabs(actual - expected) < 0.0001f;
}

} // namespace

int main()
{
    int failures = 0;
    const auto check = [&](bool actual, const char *name)
    {
        if (actual)
            return;
        std::printf("FAIL %s\n", name);
        ++failures;
    };

    gears::draw::NativeDrawInputOptions options;
    gears::draw::NativeDrawInput untouched;
    untouched.surfaceBase = 0x1234;
    check(!gears::draw::BuildNativeDrawInput(nullptr, 3, 12, true, false, 1, 0x4000, 0x11, 0x22,
                                             options, untouched),
          "a missing register snapshot is refused");
    check(untouched.surfaceBase == 0x1234, "a refused input does not partially overwrite output");

    std::array<uint32_t, gears::kGpuRegisterSnapshotDwords> registers{};
    registers[0x2000] = 1u << 16;                            // Xenos 2X surface.
    registers[0x2001] = 0x2D0u | (7u << 16) | (0x3Fu << 20); // k_2_10_10_10_FLOAT, -1 bias.
    registers[0x2002] = 0x5A0u | (1u << 16);                 // D24FS8 depth surface.
    registers[0x2104] = 0x00F0;
    registers[0x210C] = 0x01020304;
    registers[0x210D] = 0x05060708;
    registers[0x2200] = 0x11223344;
    registers[0x2201] = 0x55667788;
    registers[0x2204] = 1u << 16; // clip_disable.
    registers[0x2205] = 0x99AABBCC;
    registers[0x2206] = 0xDDEEFF00;
    registers[0x2080] = 0x12345678;
    registers[0x210F] = std::bit_cast<uint32_t>(2.5f);
    registers[0x2110] = std::bit_cast<uint32_t>(-3.0f);
    registers[0x2111] = std::bit_cast<uint32_t>(4.5f);
    registers[0x2112] = std::bit_cast<uint32_t>(-5.0f);
    registers[0x2113] = std::bit_cast<uint32_t>(0.75f);
    registers[0x2114] = std::bit_cast<uint32_t>(0.25f);

    options.msaaModel = true;
    options.hasDepthClamp = true;
    options.applyDepthBias = false;
    options.fixedViewport = true;
    options.sampleGridWidth = 1280;
    options.sampleGridHeight = 1440;
    options.targetWidth = 640;
    options.targetHeight = 360;
    options.maxViewportWidth = 1024;
    options.maxViewportHeight = 768;

    gears::draw::NativeDrawInput input;
    check(gears::draw::BuildNativeDrawInput(registers.data(), 8, 37, true, true, 2, 0x4000,
                                            0x1122334455667788ULL, 0x8877665544332211ULL, options,
                                            input),
          "a complete register snapshot produces native draw input");
    check(input.primitiveType == 8 && input.indexCount == 37 && input.indexed && input.indexIs32 &&
              input.indexEndian == 2 && input.indexGuestBase == 0x4000,
          "draw identity is carried through the native boundary");
    check(input.vertexShaderHash == 0x1122334455667788ULL &&
              input.pixelShaderHash == 0x8877665544332211ULL,
          "shader identities are carried through the native boundary");
    check(input.surfaceBase == 0x2D0 && input.colorFormat == 7 && input.colorExpBias == -1 &&
              input.depthBase == 0x5A0 && input.depthIsFloat24,
          "colour and depth target state is decoded once");
    check(input.sampleLayout.imageWidth == 1280 && input.sampleLayout.imageHeight == 720 &&
              input.sampleLayout.rasterSamples == 2 && input.sampleLayout.viewportScaleX == 1 &&
              input.sampleLayout.viewportScaleY == 1,
          "2X sample layout is preserved");
    check(input.outputMerger.colorMask == 0x00F0 && input.outputMerger.blend0 == 0x55667788 &&
              input.outputMerger.depthControl == 0x11223344 &&
              input.outputMerger.stencilRefMask == 0x05060708 &&
              input.outputMerger.stencilRefMaskBf == 0x01020304 &&
              input.outputMerger.suScModeCntl == 0x99AABBCC && input.outputMerger.depthClamp,
          "output-merger state is preserved");
    check(input.depthBias.constantFactor == 0.0f && input.depthBias.slopeFactor == 0.0f,
          "the depth-bias option can disable bias without a second decoder");
    check(input.clipControl == 0x00010000 && input.vteControl == 0xDDEEFF00 &&
              input.windowOffset == 0x12345678 && Near(input.viewportXScale, 2.5f) &&
              Near(input.viewportXOffset, -3.0f) && Near(input.viewportYScale, 4.5f) &&
              Near(input.viewportYOffset, -5.0f) && Near(input.viewportZScale, 0.75f) &&
              Near(input.viewportZOffset, 0.25f),
          "diagnostic viewport controls are copied without rereading registers");
    check(input.guestViewport.x == 0 && input.guestViewport.y == 0 &&
              input.guestViewport.w == 640 && input.guestViewport.h == 360 &&
              input.guestViewport.scissorX == 0 && input.guestViewport.scissorY == 0 &&
              input.guestViewport.scissorW == 640 && input.guestViewport.scissorH == 360,
          "the fixed viewport control arm preserves its old semantics");
    check(!input.viewportClamped && Near(input.viewport.width, 640.0f) &&
              Near(input.viewport.height, 360.0f) && input.viewport.scissorX == 0 &&
              input.viewport.scissorY == 0 && input.viewport.scissorWidth == 640 &&
              input.viewport.scissorHeight == 360,
          "host viewport and scissor are derived in the selected sample space");

    std::uint32_t publications = 0;
    std::uint64_t publishedSequence = 0;
    gears::draw::NativeFrameMaterialization published;
    {
        gears::draw::NativeFrameMaterializationRecorder recorder(
            77, 3,
            [&](std::uint64_t sequence, gears::draw::NativeFrameMaterialization result)
            {
                ++publications;
                publishedSequence = sequence;
                published = std::move(result);
            });
        recorder.MarkResolve(1);
        recorder.SetPacketIdentity(2, 0xA0012000, 0xA0010000, true);
        recorder.MarkMaterialized(2, input);
        recorder.Publish();
    }
    check(publications == 1 && publishedSequence == 77 && published.draws.size() == 3,
          "one terminal materialization result is published");
    check(
        published.draws[0].sourceOrdinal == 0 &&
            published.draws[0].outcome == gears::draw::NativeDrawMaterializationOutcome::Refused &&
            published.draws[1].sourceOrdinal == 1 &&
            published.draws[1].outcome == gears::draw::NativeDrawMaterializationOutcome::Resolve &&
            published.draws[2].sourceOrdinal == 2 &&
            published.draws[2].outcome ==
                gears::draw::NativeDrawMaterializationOutcome::Materialized &&
            published.draws[2].packetGuestAddress == 0xA0012000 &&
            published.draws[2].packetBufferBase == 0xA0010000 &&
            published.draws[2].packetFromIndirectBuffer &&
            published.draws[2].input.indexGuestBase == input.indexGuestBase,
        "refused, resolve, and materialized ordinals remain explicit");

    if (failures == 0)
        std::puts("GPU draw native-input tests passed");
    return failures != 0;
}

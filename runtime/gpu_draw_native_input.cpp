#include "gpu_draw_native_input.h"

#ifdef GEARS_HAVE_GUEST_DRAW

#include <algorithm>
#include <cstring>

namespace gears::draw
{

namespace
{

int32_t SignedColorExponent(uint32_t colorInfo)
{
    int32_t exponent = int32_t((colorInfo >> 20) & 0x3F);
    if (exponent & 0x20)
        exponent -= 64;
    return exponent;
}

} // namespace

bool BuildNativeDrawInput(const uint32_t *registerFile, uint32_t primitiveType, uint32_t indexCount,
                          bool indexed, bool indexIs32, uint32_t indexEndian,
                          uint32_t indexGuestBase, uint64_t vertexShaderHash,
                          uint64_t pixelShaderHash, const NativeDrawInputOptions &options,
                          NativeDrawInput &out)
{
    if (!registerFile || options.sampleGridWidth == 0 || options.sampleGridHeight == 0 ||
        options.targetWidth == 0 || options.targetHeight == 0 || options.maxViewportWidth == 0 ||
        options.maxViewportHeight == 0)
        return false;

    NativeDrawInput input;
    input.primitiveType = primitiveType;
    input.indexCount = indexCount;
    input.indexed = indexed;
    input.indexIs32 = indexIs32;
    input.indexEndian = indexEndian;
    input.indexGuestBase = indexGuestBase;
    input.vertexShaderHash = vertexShaderHash;
    input.pixelShaderHash = pixelShaderHash;

    const uint32_t colorInfo = registerFile[0x2001];
    input.surfaceBase = colorInfo & 0xFFF;
    input.colorFormat = (colorInfo >> 16) & 0xF;
    input.colorExpBias = SignedColorExponent(colorInfo);
    input.surfaceInfo = registerFile[0x2000];
    input.depthBase = registerFile[0x2002] & 0xFFF;
    input.depthIsFloat24 = ((registerFile[0x2002] >> 16) & 1) != 0;
    const uint32_t msaaSamples = options.msaaModel ? ((input.surfaceInfo >> 16) & 3) : 0;
    input.sampleLayout =
        DeriveDrawSampleLayout(msaaSamples, options.sampleGridWidth, options.sampleGridHeight);

    input.outputMerger.colorMask = registerFile[0x2104];
    input.outputMerger.blend0 = registerFile[0x2201];
    input.outputMerger.depthControl = registerFile[0x2200];
    input.outputMerger.stencilRefMask = registerFile[0x210D];
    input.outputMerger.stencilRefMaskBf = registerFile[0x210C];
    input.outputMerger.suScModeCntl = registerFile[0x2205];
    input.outputMerger.polygonal = IsPrimitivePolygonal(registerFile);
    input.outputMerger.depthClamp =
        options.hasDepthClamp && ((registerFile[0x2204] >> 16) & 1) != 0;

    input.depthBias = DeriveDepthBias(registerFile, input.outputMerger.polygonal);
    if (!options.applyDepthBias)
        input.depthBias = {};

    if (!DeriveViewport(registerFile, input.guestViewport))
        return false;
    if (options.fixedViewport)
    {
        input.guestViewport.x = input.guestViewport.y = 0;
        input.guestViewport.scissorX = input.guestViewport.scissorY = 0;
        input.guestViewport.w = input.guestViewport.scissorW = options.targetWidth;
        input.guestViewport.h = input.guestViewport.scissorH = options.targetHeight;
        input.guestViewport.zMin = 0.0f;
        input.guestViewport.zMax = 1.0f;
    }

    input.viewportClamped = input.guestViewport.w > options.maxViewportWidth ||
                            input.guestViewport.h > options.maxViewportHeight;
    const uint32_t sx = options.msaaModel ? input.sampleLayout.viewportScaleX : 1u;
    const uint32_t sy = options.msaaModel ? input.sampleLayout.viewportScaleY : 1u;
    input.viewport.x = float(input.guestViewport.x * sx);
    input.viewport.y = float(input.guestViewport.y * sy);
    input.viewport.width = float(std::min(input.guestViewport.w * sx, options.maxViewportWidth));
    input.viewport.height = float(std::min(input.guestViewport.h * sy, options.maxViewportHeight));
    input.viewport.minDepth = input.guestViewport.zMin;
    input.viewport.maxDepth = input.guestViewport.zMax;

    const uint32_t targetWidth =
        options.msaaModel ? input.sampleLayout.imageWidth : options.targetWidth;
    const uint32_t targetHeight =
        options.msaaModel ? input.sampleLayout.imageHeight : options.targetHeight;
    const uint32_t scissorX = input.guestViewport.scissorX * sx;
    const uint32_t scissorY = input.guestViewport.scissorY * sy;
    const uint32_t boundedX = std::min(scissorX, targetWidth);
    const uint32_t boundedY = std::min(scissorY, targetHeight);
    input.viewport.scissorX = boundedX;
    input.viewport.scissorY = boundedY;
    input.viewport.scissorWidth =
        std::min(input.guestViewport.scissorW * sx, targetWidth - boundedX);
    input.viewport.scissorHeight =
        std::min(input.guestViewport.scissorH * sy, targetHeight - boundedY);

    input.clipControl = registerFile[0x2204];
    input.vteControl = registerFile[0x2206];
    input.windowOffset = registerFile[0x2080];
    std::memcpy(&input.viewportXScale, &registerFile[0x210F], sizeof(float));
    std::memcpy(&input.viewportXOffset, &registerFile[0x2110], sizeof(float));
    std::memcpy(&input.viewportYScale, &registerFile[0x2111], sizeof(float));
    std::memcpy(&input.viewportYOffset, &registerFile[0x2112], sizeof(float));
    std::memcpy(&input.viewportZScale, &registerFile[0x2113], sizeof(float));
    std::memcpy(&input.viewportZOffset, &registerFile[0x2114], sizeof(float));

    out = input;
    return true;
}

} // namespace gears::draw

#endif

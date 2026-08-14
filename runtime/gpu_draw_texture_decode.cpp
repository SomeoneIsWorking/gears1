#include "gpu_draw_texture_decode.h"

#ifdef GEARS_HAVE_GUEST_DRAW

#include <algorithm>
#include <cstring>

#include "xenia/base/math.h"
#include "xenia/gpu/texture_address.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/xenos.h"

namespace gears::draw
{
namespace
{

namespace texture_address = xe::gpu::texture_address;
namespace texture_util = xe::gpu::texture_util;
namespace xenos = xe::gpu::xenos;

uint32_t EndianOffsetXor(xenos::Endian endian)
{
    switch (endian)
    {
        case xenos::Endian::k8in16: return 1;
        case xenos::Endian::k8in32: return 3;
        case xenos::Endian::k16in32: return 2;
        default: return 0;
    }
}

struct HostFormat
{
    TexHostFormat format = TexHostFormat::kUnsupported;
    uint8_t swizzle[4] = {0, 1, 2, 3};
    const char* unsupportedWhy = nullptr;
};

#define SW(a, b, c, d) {uint8_t(a), uint8_t(b), uint8_t(c), uint8_t(d)}

HostFormat MapFormat(xenos::TextureFormat format)
{
    using TF = xenos::TextureFormat;
    switch (format)
    {
        case TF::k_8:
        case TF::k_8_A:      return {TexHostFormat::kR8Unorm, SW(0, 0, 0, 0)};
        case TF::k_8_8:      return {TexHostFormat::kR8G8Unorm, SW(0, 1, 1, 1)};
        case TF::k_8_8_8_8:  return {TexHostFormat::kR8G8B8A8Unorm, SW(0, 1, 2, 3)};
        case TF::k_5_6_5:    return {TexHostFormat::kR5G6B5Pack16, SW(2, 1, 0, 2)};
        case TF::k_1_5_5_5:  return {TexHostFormat::kA1R5G5B5Pack16, SW(2, 1, 0, 3)};
        case TF::k_2_10_10_10:
            return {TexHostFormat::kA2B10G10R10Pack32, SW(0, 1, 2, 3)};
        case TF::k_16:       return {TexHostFormat::kR16Unorm, SW(0, 0, 0, 0)};
        case TF::k_16_16:    return {TexHostFormat::kR16G16Unorm, SW(0, 1, 1, 1)};
        case TF::k_16_16_16_16:
            return {TexHostFormat::kR16G16B16A16Unorm, SW(0, 1, 2, 3)};
        case TF::k_16_FLOAT: return {TexHostFormat::kR16Sfloat, SW(0, 0, 0, 0)};
        case TF::k_16_16_FLOAT:
            return {TexHostFormat::kR16G16Sfloat, SW(0, 1, 1, 1)};
        case TF::k_16_16_16_16_FLOAT:
            return {TexHostFormat::kR16G16B16A16Sfloat, SW(0, 1, 2, 3)};
        case TF::k_32_FLOAT: return {TexHostFormat::kR32Sfloat, SW(0, 0, 0, 0)};
        case TF::k_32_32_FLOAT:
            return {TexHostFormat::kR32G32Sfloat, SW(0, 1, 1, 1)};
        case TF::k_32_32_32_32_FLOAT:
            return {TexHostFormat::kR32G32B32A32Sfloat, SW(0, 1, 2, 3)};
        case TF::k_DXT1:     return {TexHostFormat::kBc1RgbaUnorm, SW(0, 1, 2, 3)};
        case TF::k_DXT2_3:   return {TexHostFormat::kBc2Unorm, SW(0, 1, 2, 3)};
        case TF::k_DXT4_5:   return {TexHostFormat::kBc3Unorm, SW(0, 1, 2, 3)};
        case TF::k_DXT5A:    return {TexHostFormat::kBc4Unorm, SW(0, 0, 0, 0)};
        case TF::k_DXN:      return {TexHostFormat::kBc5Unorm, SW(0, 1, 1, 1)};
        default:
            return {TexHostFormat::kUnsupported, SW(0, 1, 2, 3),
                    "no host format mapping"};
    }
}

#undef SW

bool SpanFits(uint32_t address, uint32_t extent, uint64_t guestSize)
{
    return extent == 0 || uint64_t(address) + uint64_t(extent) <= guestSize;
}

uint64_t SourceBlockOffset(bool tiled, bool is3D, uint32_t x, uint32_t y,
                           uint32_t z, uint32_t pitchBlocks,
                           const texture_util::TextureGuestLayout::Level& layout,
                           uint32_t bytesPerBlock)
{
    if (tiled)
    {
        const uint32_t bpbLog2 = xe::log2_floor(bytesPerBlock);
        if (is3D)
            return uint64_t(texture_address::Tiled3D(
                int32_t(x), int32_t(y), int32_t(z), pitchBlocks,
                layout.z_slice_stride_block_rows, bpbLog2));
        return uint64_t(uint32_t(texture_address::Tiled2D(
            int32_t(x), int32_t(y), pitchBlocks, bpbLog2)));
    }
    return uint64_t(z) * layout.row_pitch_bytes *
               layout.z_slice_stride_block_rows +
           uint64_t(y) * layout.row_pitch_bytes +
           uint64_t(x) * bytesPerBlock;
}

} // namespace

bool DecodeGuestTexture(const uint32_t* fetch6, const uint8_t* guestBase,
                        uint64_t guestSize, bool wantData, GuestTexture& out)
{
    xenos::xe_gpu_texture_fetch_t fetch{};
    std::memcpy(&fetch, fetch6, sizeof(fetch));
    if (fetch.type != xenos::FetchConstantType::kTexture)
        return false;

    uint32_t w1 = 0, h1 = 0, d1 = 0, basePage = 0, mipPage = 0;
    texture_util::GetSubresourcesFromFetchConstant(
        fetch, &w1, &h1, &d1, &basePage, &mipPage, &out.mipMin, &out.mipMax);

    out.formatRaw = uint32_t(fetch.format);
    const xenos::TextureFormat baseFormat = xe::gpu::GetBaseFormat(fetch.format);
    out.baseFormatRaw = uint32_t(baseFormat);
    out.formatName = xe::gpu::FormatInfo::GetName(uint32_t(fetch.format));
    out.dimension = uint32_t(fetch.dimension);
    out.width = w1 + 1;
    out.height = fetch.dimension == xenos::DataDimension::k1D ? 1 : h1 + 1;
    out.depthOrArraySize = d1 + 1;
    out.tiled = fetch.tiled != 0;
    out.packedMips = fetch.packed_mips != 0;
    out.baseAddress = basePage << 12;
    out.mipAddress = mipPage << 12;
    out.endian = uint32_t(fetch.endianness);
    out.guestSwizzle = fetch.swizzle;

    const HostFormat host = MapFormat(baseFormat);
    out.hostFormat = host.format;
    for (uint32_t i = 0; i < 4; ++i)
    {
        const uint32_t guestComponent = (fetch.swizzle >> (3 * i)) & 0b111;
        out.hostSwizzle[i] = guestComponent >= 4
            ? uint8_t(guestComponent & 0b101) : host.swizzle[guestComponent];
    }

    if (basePage == 0)
    {
        out.skipReason = "base level not stored (mip_min_level > 0)";
        return true;
    }
    if (out.mipMax != 0 && mipPage == 0)
    {
        out.skipReason = "mip levels declared without mip storage";
        return true;
    }
    if (host.format == TexHostFormat::kUnsupported)
    {
        out.skipReason = host.unsupportedWhy;
        return true;
    }
    const xe::gpu::FormatInfo* formatInfo = xe::gpu::FormatInfo::Get(baseFormat);
    if (!formatInfo || !formatInfo->bytes_per_block())
    {
        out.skipReason = "no block size for format";
        return true;
    }
    out.blockWidth = formatInfo->block_width;
    out.blockHeight = formatInfo->block_height;
    out.bytesPerBlock = formatInfo->bytes_per_block();

    const bool is3D = fetch.dimension == xenos::DataDimension::k3D;
    const bool isCube = fetch.dimension == xenos::DataDimension::kCube;
    out.layers = is3D ? 1 : (isCube ? 6 :
        (fetch.dimension == xenos::DataDimension::k2DOrStacked
             ? out.depthOrArraySize : 1));
    out.depth3D = is3D ? out.depthOrArraySize : 1;

    const texture_util::TextureGuestLayout layout =
        texture_util::GetGuestTextureLayout(
            fetch.dimension, fetch.pitch, out.width, out.height,
            out.depthOrArraySize, out.tiled, baseFormat, out.packedMips,
            /*has_base=*/true, out.mipMax);
    if (!layout.base.row_pitch_bytes)
    {
        out.skipReason = "degenerate guest layout";
        return true;
    }
    out.baseGuestExtentBytes = layout.base.level_data_extent_bytes;
    out.mipGuestExtentBytes = layout.mips_total_extent_bytes;

    if (!SpanFits(out.baseAddress, out.baseGuestExtentBytes, guestSize) ||
        !SpanFits(out.mipAddress, out.mipGuestExtentBytes, guestSize))
    {
        out.skipReason = "texture data outside the guest window";
        return true;
    }
    if (!wantData)
        return true;

    uint64_t total = 0;
    out.levels.reserve(out.mipMax + 1);
    for (uint32_t level = 0; level <= out.mipMax; ++level)
    {
        GuestTextureLevel decoded{};
        decoded.width = std::max(out.width >> level, 1u);
        decoded.height = std::max(out.height >> level, 1u);
        decoded.depth = std::max(out.depth3D >> level, 1u);
        decoded.blocksX = (decoded.width + out.blockWidth - 1) / out.blockWidth;
        decoded.blocksY = (decoded.height + out.blockHeight - 1) / out.blockHeight;
        decoded.rowPitchBytes = decoded.blocksX * out.bytesPerBlock;
        // Vulkan requires bufferOffset to be a multiple of 4 and of the
        // format's texel-block size. Xenos formats use power-of-two block
        // sizes, so the larger alignment satisfies both.
        const uint64_t alignment = std::max(4u, out.bytesPerBlock);
        total = (total + alignment - 1) & ~(alignment - 1);
        decoded.dataOffset = total;
        decoded.dataSize = uint64_t(decoded.rowPitchBytes) * decoded.blocksY *
                           decoded.depth * out.layers;
        total += decoded.dataSize;
        out.levels.push_back(decoded);
    }
    if (total == 0 || total > (uint64_t(256) << 20))
    {
        out.skipReason = "implausible decoded size";
        out.levels.clear();
        return true;
    }

    out.data.resize(size_t(total));
    const uint32_t endianXor = EndianOffsetXor(fetch.endianness);
    for (uint32_t level = 0; level <= out.mipMax; ++level)
    {
        const bool base = level == 0;
        const uint32_t storedLevel = base ? 0 : std::min(level, layout.packed_level);
        const auto& sourceLayout = base ? layout.base : layout.mips[storedLevel];
        const uint32_t sourceAddress = base ? out.baseAddress : out.mipAddress;
        const uint64_t sourceLevelOffset = base ? 0 : layout.mip_offsets_bytes[storedLevel];
        const uint32_t pitchBlocks = sourceLayout.row_pitch_bytes / out.bytesPerBlock;

        uint32_t packedX = 0, packedY = 0, packedZ = 0;
        if (level >= layout.packed_level)
            texture_util::GetPackedMipOffset(out.width, out.height, out.depth3D,
                                             baseFormat, level,
                                             packedX, packedY, packedZ);

        const GuestTextureLevel& decoded = out.levels[level];
        uint8_t* destination = out.data.data() + decoded.dataOffset;
        for (uint32_t layer = 0; layer < out.layers; ++layer)
        {
            const uint64_t layerOffset =
                uint64_t(layer) * sourceLayout.array_slice_stride_bytes;
            for (uint32_t z = 0; z < decoded.depth; ++z)
            {
                for (uint32_t y = 0; y < decoded.blocksY; ++y)
                {
                    for (uint32_t x = 0; x < decoded.blocksX; ++x)
                    {
                        const uint64_t sourceBlock = SourceBlockOffset(
                            out.tiled, is3D, x + packedX, y + packedY,
                            z + packedZ, pitchBlocks, sourceLayout,
                            out.bytesPerBlock);
                        const uint64_t sourceOffset = sourceLevelOffset +
                            layerOffset + sourceBlock;
                        const uint8_t* source = guestBase + sourceAddress;
                        for (uint32_t byte = 0; byte < out.bytesPerBlock; ++byte)
                            *destination++ = source[(sourceOffset + byte) ^ endianXor];
                    }
                }
            }
        }
    }
    return true;
}

} // namespace gears::draw

#endif // GEARS_HAVE_GUEST_DRAW

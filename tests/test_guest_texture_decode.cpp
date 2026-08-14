#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "gpu_draw_texture_decode.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/xenos.h"

namespace
{

namespace texture_util = xe::gpu::texture_util;
namespace xenos = xe::gpu::xenos;

int failures = 0;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::printf("FAIL %s\n", message);
        ++failures;
    }
}

xenos::xe_gpu_texture_fetch_t MakeFetch()
{
    xenos::xe_gpu_texture_fetch_t fetch{};
    fetch.type = xenos::FetchConstantType::kTexture;
    fetch.pitch = 3; // 96 aligned texels / 32 for a 65-texel texture.
    fetch.format = xenos::TextureFormat::k_8;
    fetch.endianness = xenos::Endian::kNone;
    fetch.base_address = 1;
    fetch.size_2d.width = 64;
    fetch.size_2d.height = 64;
    fetch.swizzle = 0x688;
    fetch.mip_max_level = 6;
    fetch.dimension = xenos::DataDimension::k2DOrStacked;
    fetch.packed_mips = 1;
    fetch.mip_address = 64;
    return fetch;
}

void TestAuthoredMipStorageIsDecoded()
{
    xenos::xe_gpu_texture_fetch_t fetch = MakeFetch();
    const auto layout = texture_util::GetGuestTextureLayout(
        fetch.dimension, fetch.pitch, 65, 65, 1, false, fetch.format,
        true, true, 6);
    std::vector<uint8_t> guest(4 << 20, 0);
    std::fill_n(guest.data() + (fetch.base_address << 12),
                layout.base.level_data_extent_bytes, 0x11);
    std::fill_n(guest.data() + (fetch.mip_address << 12),
                layout.mips_total_extent_bytes, 0x22);

    gears::draw::GuestTexture decoded;
    Check(gears::draw::DecodeGuestTexture(
              &fetch.dword_0, guest.data(), guest.size(), true, decoded),
          "a texture fetch is accepted");
    Check(decoded.skipReason == nullptr,
          "a complete base plus authored mip allocation is not skipped");
    Check(decoded.levels.size() == 7,
          "mip_max_level 6 produces all seven host levels");
    Check(decoded.baseGuestExtentBytes == layout.base.level_data_extent_bytes &&
              decoded.mipGuestExtentBytes == layout.mips_total_extent_bytes,
          "header reports both disjoint guest spans exactly");

    for (size_t level = 0; level < decoded.levels.size(); ++level)
    {
        const auto& current = decoded.levels[level];
        const uint64_t end = current.dataOffset + current.dataSize;
        const uint8_t expected = level == 0 ? 0x11 : 0x22;
        Check(current.width == std::max(65u >> level, 1u) &&
                  current.height == std::max(65u >> level, 1u),
              "each decoded level has its logical host extent");
        Check((current.dataOffset & 3) == 0,
              "every staging level starts at a Vulkan-valid alignment");
        Check(std::all_of(decoded.data.begin() + current.dataOffset,
                          decoded.data.begin() + end,
                          [expected](uint8_t value) { return value == expected; }),
              level == 0
                  ? "level zero comes only from base storage"
                  : "every authored mip comes only from mip storage");
    }
}

void TestOutOfWindowMipStorageRefuses()
{
    xenos::xe_gpu_texture_fetch_t fetch = MakeFetch();
    fetch.mip_address = 255;
    std::vector<uint8_t> guest(64 << 10, 0);
    gears::draw::GuestTexture decoded;
    Check(gears::draw::DecodeGuestTexture(
              &fetch.dword_0, guest.data(), guest.size(), false, decoded),
          "the descriptor is still identified as a texture fetch");
    Check(decoded.skipReason != nullptr &&
              std::strcmp(decoded.skipReason,
                          "texture data outside the guest window") == 0,
          "out-of-window authored mip storage is reported, never read silently");
}

} // namespace

int main()
{
    TestAuthoredMipStorageIsDecoded();
    TestOutOfWindowMipStorageRefuses();
    if (failures == 0)
    {
        std::printf("all guest texture decode tests passed\n");
        return 0;
    }
    std::printf("%d guest texture decode test(s) FAILED\n", failures);
    return 1;
}

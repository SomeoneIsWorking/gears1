#pragma once

#include <cstdint>

namespace gears::titles::gears1
{

namespace color_write_gamma
{

constexpr std::uint32_t kDirtyMaskOffset = 0x18;
constexpr std::uint32_t kSurfaceInfoOffset = 0x2800;
constexpr std::uint32_t kColorDescriptorOffset = 0x2804;
constexpr std::uint32_t kRequestedStateOffset = 0x2DEC;
constexpr std::uint32_t kColorTargetObjectOffset = 0x2F88;
constexpr std::uint32_t kObjectDescriptorOffset = 0x1C;
constexpr std::uint64_t kDirtyMask = std::uint64_t{1} << 37;

[[nodiscard]] constexpr bool SupportsGammaSelection(std::uint32_t format)
{
    return format == 2 || format == 3 || format == 10 || format == 12;
}

[[nodiscard]] constexpr std::uint32_t SelectSurfaceFormat(std::uint32_t format,
                                                          std::uint64_t requested)
{
    const std::uint32_t mask = static_cast<std::uint32_t>(requested - 1);
    const std::uint32_t firstPair = (format >> 1) - 3;
    const std::uint32_t secondPair = (format + 3) << 1;
    return ((firstPair & mask) | (secondPair & ~mask)) & 0xF;
}

} // namespace color_write_gamma

template <typename Memory>
[[nodiscard]] bool ApplyNativeColorWriteGammaState(Memory &memory, std::uint32_t device,
                                                   std::uint64_t requested)
{
    using namespace color_write_gamma;
    memory.Write32(device + kRequestedStateOffset, static_cast<std::uint32_t>(requested));

    const std::uint32_t target = memory.Read32(device + kColorTargetObjectOffset);
    if (target == 0)
        return false;

    const std::uint32_t descriptor = memory.Read32(target + kObjectDescriptorOffset);
    const std::uint32_t format = (descriptor >> 16) & 0xF;
    if (!SupportsGammaSelection(format) || (((descriptor >> 19) & 1u) ^ requested) == 0)
        return false;

    const std::uint32_t selectedFormat = SelectSurfaceFormat(format, requested);
    const std::uint32_t selectedBits = selectedFormat << 16;
    const std::uint32_t objectDescriptor = (descriptor & 0xFFF0FFFFu) | selectedBits;
    memory.Write32(target + kObjectDescriptorOffset, objectDescriptor);

    const std::uint32_t deviceDescriptor = memory.Read32(device + kColorDescriptorOffset);
    memory.Write32(device + kColorDescriptorOffset,
                   (deviceDescriptor & 0xFFF0FFFFu) | selectedBits);
    memory.Write64(device + kDirtyMaskOffset,
                   memory.Read64(device + kDirtyMaskOffset) | kDirtyMask);
    return true;
}

} // namespace gears::titles::gears1

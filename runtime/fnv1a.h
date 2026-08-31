#pragma once

#include <cstdint>
#include <span>

namespace gears
{

[[nodiscard]] inline constexpr std::uint64_t Fnv1a64(std::span<const std::uint8_t> bytes)
{
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (const std::uint8_t byte : bytes)
    {
        hash ^= byte;
        hash *= 0x100000001B3ull;
    }
    return hash;
}

} // namespace gears

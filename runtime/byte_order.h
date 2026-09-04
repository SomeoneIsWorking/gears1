#pragma once

#include <cstdint>
#include <type_traits>

// Xbox 360 guest data is big-endian. Keep this tiny host-native primitive in
// the runtime instead of depending on a retired translation-tool utility.
template <typename T>
    requires(std::is_integral_v<T>)
[[nodiscard]] constexpr T ByteSwap(T value) noexcept
{
    using Unsigned = std::make_unsigned_t<T>;
    const Unsigned bits = static_cast<Unsigned>(value);
    if constexpr (sizeof(T) == 1)
    {
        return value;
    }
    else if constexpr (sizeof(T) == 2)
    {
        return static_cast<T>(__builtin_bswap16(bits));
    }
    else if constexpr (sizeof(T) == 4)
    {
        return static_cast<T>(__builtin_bswap32(bits));
    }
    else
    {
        static_assert(sizeof(T) == 8, "ByteSwap supports 8-, 16-, 32-, and 64-bit integers");
        return static_cast<T>(__builtin_bswap64(bits));
    }
}

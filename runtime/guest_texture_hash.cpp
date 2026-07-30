#include "guest_texture_hash.h"

namespace gears
{

uint64_t HashGuestTexture(const uint8_t* bytes, size_t length)
{
    // The offset basis and prime are FNV-1a's 64-bit constants.
    uint64_t hash = 0xcbf29ce484222325ull;
    if (bytes == nullptr)
        return hash;
    for (size_t i = 0; i < length; ++i)
    {
        hash ^= uint64_t(bytes[i]);
        hash *= 0x100000001b3ull;
    }
    return hash;
}

} // namespace gears

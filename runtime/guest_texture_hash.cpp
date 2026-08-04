#include "guest_texture_hash.h"

#include <cstring>

namespace gears
{

namespace
{

// Murmur3's 64-bit finalizer. It is what turns the accumulator into a value whose
// bits are all sensitive to the whole input: without it, a word-at-a-time FNV-1a
// leaves the bits of the last word barely mixed, and two textures differing only
// near their end can land closer together than they should.
inline uint64_t Avalanche(uint64_t h)
{
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return h;
}

} // namespace

uint64_t HashGuestTexture(const uint8_t* bytes, size_t length)
{
    // The offset basis and prime are FNV-1a's 64-bit constants; the mixing runs
    // EIGHT BYTES AT A TIME rather than one, which is the whole reason this is not
    // the textbook loop. Coverage is identical -- every byte enters the
    // accumulator, and tests/test_guest_texture_hash.cpp flips each byte of a span
    // in turn to prove it -- but this now runs on every distinct texture of every
    // frame, where the byte-at-a-time version was too slow to be the default.
    uint64_t hash = 0xCBF29CE484222325ull;
    if (bytes == nullptr)
        return hash;

    size_t i = 0;
    for (; i + 8 <= length; i += 8)
    {
        uint64_t word;
        std::memcpy(&word, bytes + i, sizeof(word)); // no alignment assumption
        hash ^= word;
        hash *= 0x100000001B3ull;
        // Without this, the multiply only propagates bits upward, so a change in
        // the top byte of a word can only affect the top bits of the accumulator
        // -- and for the LAST word nothing follows to spread it. One shift-xor per
        // word costs nothing and removes that blind spot.
        hash ^= hash >> 29;
    }
    // The tail, byte at a time. A texture whose extent is not a multiple of 8 --
    // an odd width, a small mip -- must still have every byte counted, and the
    // length is folded in so a trailing zero byte is not the same as no byte.
    for (; i < length; ++i)
    {
        hash ^= uint64_t(bytes[i]);
        hash *= 0x100000001B3ull;
    }
    hash ^= uint64_t(length);
    return Avalanche(hash);
}

} // namespace gears

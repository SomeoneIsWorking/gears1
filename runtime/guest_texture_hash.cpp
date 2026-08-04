#include "guest_texture_hash.h"

// XXH3, from the xxHash the recompiler already vendors. Header-only (XXH_INLINE_ALL),
// so this costs a include path and no link dependency.
//
// WHY NOT THE HAND-ROLLED LOOP THIS REPLACED: the renderer hashes every distinct
// texture of every frame -- about 15 MiB on a gameplay frame -- and the mixing
// width is what that costs. Measured on this machine over 15 MiB, warm: the
// eight-bytes-at-a-time FNV-1a ran at 4.8 GB/s (3.26 ms), XXH3 at 55.0 GB/s
// (0.29 ms). Coverage is unchanged: XXH3 reads every byte, and
// tests/test_guest_texture_hash.cpp still flips each byte of a span in turn.
#define XXH_INLINE_ALL
#include "xxhash.h"

namespace gears
{

uint64_t HashGuestTexture(const uint8_t* bytes, size_t length)
{
    if (bytes == nullptr)
        return 0;
    return uint64_t(XXH3_64bits(bytes, length));
}

} // namespace gears

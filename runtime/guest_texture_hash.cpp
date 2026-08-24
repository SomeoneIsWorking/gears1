#include "guest_texture_hash.h"

// XXH3, from the xxHash the recompiler already vendors. On x86 the vendor's
// dispatcher selects SSE2, AVX2, or AVX-512 at runtime, so a portable binary is
// not permanently limited to the generic x86-64 SSE2 baseline. Other
// architectures retain xxHash's normal compile-time SIMD selection.
//
// WHY NOT THE HAND-ROLLED LOOP THIS REPLACED: the renderer hashes every distinct
// texture of every frame -- about 15 MiB on a gameplay frame -- and the mixing
// width is what that costs. Measured on this machine over 15 MiB, warm: the
// eight-bytes-at-a-time FNV-1a ran at 4.8 GB/s (3.26 ms), XXH3 at 55.0 GB/s
// (0.29 ms). Coverage is unchanged: XXH3 reads every byte, and
// tests/test_guest_texture_hash.cpp still flips each byte of a span in turn.
#define XXH_STATIC_LINKING_ONLY
#if defined(GEARS_XXHASH_X86_DISPATCH)
#include "xxh_x86dispatch.h"
#else
#include "xxhash.h"
#endif

namespace gears
{

uint64_t HashGuestTexture(const uint8_t *bytes, size_t length)
{
    if (bytes == nullptr)
        return 0;
    return uint64_t(XXH3_64bits(bytes, length));
}

uint64_t HashGuestTextureParts(const uint8_t *base, size_t baseLength, const uint8_t *mips,
                               size_t mipLength)
{
    if ((base == nullptr && baseLength != 0) || (mips == nullptr && mipLength != 0))
        return 0;
    XXH3_state_t state;
    XXH3_64bits_reset(&state);
    if (baseLength != 0)
        XXH3_64bits_update(&state, base, baseLength);
    if (mipLength != 0)
        XXH3_64bits_update(&state, mips, mipLength);
    return uint64_t(XXH3_64bits_digest(&state));
}

} // namespace gears

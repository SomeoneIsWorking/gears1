// Detecting that a guest texture's CONTENTS changed under a cache keyed on its
// descriptor.
//
// The renderer caches uploaded textures against the texture fetch constant -- base
// address, dimensions, format, swizzle. That key is stable when the guest overwrites
// the pixels at the same address with the same descriptor, which is exactly what a
// title does with a render-to-texture target, an animated UI element or a streamed
// mip. The cached image then keeps being sampled long after the data behind it
// changed, and nothing invalidates it.
//
// WHY A FULL HASH AND NOT A SAMPLE. Hashing a few bytes from the start, middle and
// end is cheap and unsound: a texture whose changed region happens to miss the
// sampled offsets is reported unchanged, and the symptom is a stale image on screen
// with no error anywhere. A detector that can silently miss the thing it exists to
// find is worse than no detector, because the cache then looks validated. So this
// covers every byte, and the cost is measured rather than assumed -- see the report
// in gpu_draw.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gears
{

// XXH3 over the whole span. Every byte enters the hash: the speed came from the
// mixing, never from looking at less of the texture, which is the trade this file
// refuses. See the .cpp for the measured rate.
uint64_t HashGuestTexture(const uint8_t* bytes, size_t length);

// Whether a cached upload is still good. Separated from the hashing so the policy
// reads as a policy: a cache entry survives exactly while its bytes are unchanged.
inline bool GuestTextureUnchanged(uint64_t cachedHash, uint64_t currentHash)
{
    return cachedHash == currentHash;
}

} // namespace gears

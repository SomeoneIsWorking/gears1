// Tests for guest-texture change detection.
//
// The renderer caches uploaded textures against the fetch constant -- base address,
// dimensions, format, swizzle -- and that key does not change when the guest
// overwrites the pixels at the same address. A render-to-texture target, an animated
// UI element or a streamed mip all do exactly that, so the cached image goes on being
// sampled after its data changed and nothing invalidates it.
//
// The property that matters here is COVERAGE. A cheap hash over a few sampled
// offsets would pass a naive test and silently miss a change that happens to fall
// between the samples -- and the symptom is a stale texture on screen with no error
// anywhere, which reads as a rendering bug rather than a cache bug. So the tests
// below change one byte at a time across the whole span, including the interior,
// which a sampled hash would fail.

#include <cstdio>
#include <cstdint>
#include <vector>

#include "guest_texture_hash.h"

namespace
{

int g_failures = 0;

void Check(bool ok, const char *what)
{
    if (!ok)
    {
        printf("FAIL %s\n", what);
        ++g_failures;
    }
}

using gears::GuestTextureUnchanged;
using gears::HashGuestTexture;
using gears::HashGuestTextureParts;

void TestSameBytesHashTheSame()
{
    const std::vector<uint8_t> a(4096, 0x7F);
    const std::vector<uint8_t> b(4096, 0x7F);
    Check(HashGuestTexture(a.data(), a.size()) == HashGuestTexture(b.data(), b.size()),
          "identical contents hash identically, so an unchanged texture is not"
          " re-uploaded every frame");
    Check(GuestTextureUnchanged(HashGuestTexture(a.data(), a.size()),
                                HashGuestTexture(b.data(), b.size())),
          "and the policy reports it unchanged");
}

// THE POINT OF THE FILE. Every byte must matter, including ones a sampled hash
// would step over.
void TestEverySinglePositionIsCovered()
{
    constexpr size_t kSize = 1024;
    std::vector<uint8_t> base(kSize, 0x11);
    const uint64_t baseHash = HashGuestTexture(base.data(), kSize);

    size_t missed = 0;
    for (size_t i = 0; i < kSize; ++i)
    {
        std::vector<uint8_t> changed = base;
        changed[i] ^= 0xFF;
        if (HashGuestTexture(changed.data(), kSize) == baseHash)
            ++missed;
    }
    Check(missed == 0, "a one-byte change at EVERY position changes the hash -- a hash that"
                       " sampled a few offsets would miss the interior ones and report a modified"
                       " texture as unchanged");
    if (missed != 0)
        printf("  %zu of %zu positions were invisible to the hash\n", missed, kSize);
}

// A single flipped bit, not a whole byte: the weakest real change.
void TestSingleBitChangeIsSeen()
{
    std::vector<uint8_t> a(512, 0);
    const uint64_t before = HashGuestTexture(a.data(), a.size());
    a[300] = 0x01;
    Check(HashGuestTexture(a.data(), a.size()) != before,
          "one bit set in the middle changes the hash");
    Check(!GuestTextureUnchanged(before, HashGuestTexture(a.data(), a.size())),
          "and the policy reports it changed, so the cache re-uploads");
}

void TestEveryMipByteIsCovered()
{
    std::vector<uint8_t> base(256, 0x21);
    std::vector<uint8_t> mips(1024, 0x42);
    const uint64_t original =
        HashGuestTextureParts(base.data(), base.size(), mips.data(), mips.size());
    size_t missed = 0;
    for (size_t i = 0; i < mips.size(); ++i)
    {
        mips[i] ^= 1;
        missed +=
            HashGuestTextureParts(base.data(), base.size(), mips.data(), mips.size()) == original;
        mips[i] ^= 1;
    }
    Check(missed == 0, "every byte in disjoint mip storage changes the cache identity");
    if (missed != 0)
        printf("  %zu of %zu mip positions were invisible to the hash\n", missed, mips.size());
}

void TestDisjointPartsMatchContiguousIdentity()
{
    std::vector<uint8_t> base(513);
    std::vector<uint8_t> mips(271);
    for (size_t i = 0; i < base.size(); ++i)
        base[i] = uint8_t(i * 17u + 3u);
    for (size_t i = 0; i < mips.size(); ++i)
        mips[i] = uint8_t(i * 29u + 5u);
    std::vector<uint8_t> contiguous = base;
    contiguous.insert(contiguous.end(), mips.begin(), mips.end());
    Check(HashGuestTextureParts(base.data(), base.size(), mips.data(), mips.size()) ==
              HashGuestTexture(contiguous.data(), contiguous.size()),
          "disjoint base and mip updates produce the same identity as the concatenated bytes");
}

// Length is part of the identity: a texture that grew must not be mistaken for the
// same data.
void TestLengthMatters()
{
    const std::vector<uint8_t> bytes(1000, 0xAB);
    Check(HashGuestTexture(bytes.data(), 500) != HashGuestTexture(bytes.data(), 1000),
          "the same buffer hashed over different lengths differs, so a resized"
          " texture is not treated as unchanged");
}

// Reordering must not collapse: two textures holding the same bytes in a different
// order are different images.
void TestOrderMatters()
{
    const std::vector<uint8_t> a = {1, 2, 3, 4};
    const std::vector<uint8_t> b = {4, 3, 2, 1};
    Check(HashGuestTexture(a.data(), a.size()) != HashGuestTexture(b.data(), b.size()),
          "the same bytes in a different order hash differently");
}

// Degenerate inputs must not crash or claim a match with real data.
void TestDegenerateInputs()
{
    Check(HashGuestTexture(nullptr, 0) == HashGuestTexture(nullptr, 100),
          "a null pointer yields the basis regardless of the claimed length, rather"
          " than reading through it");
    const std::vector<uint8_t> one = {0};
    Check(HashGuestTexture(one.data(), 0) != HashGuestTexture(one.data(), 1),
          "an empty span and a one-byte span differ");
}

} // namespace

int main()
{
    TestSameBytesHashTheSame();
    TestEverySinglePositionIsCovered();
    TestSingleBitChangeIsSeen();
    TestEveryMipByteIsCovered();
    TestDisjointPartsMatchContiguousIdentity();
    TestLengthMatters();
    TestOrderMatters();
    TestDegenerateInputs();

    if (g_failures == 0)
    {
        printf("all guest texture hash tests passed\n");
        return 0;
    }
    printf("%d guest texture hash test(s) FAILED\n", g_failures);
    return 1;
}

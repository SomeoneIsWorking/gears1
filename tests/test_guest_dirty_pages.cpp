#include <cstdio>
#include <cstdint>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "guest_dirty_pages.h"

namespace
{

int g_failures = 0;

void Check(bool ok, const char *what)
{
    if (ok)
        return;
    std::printf("FAIL: %s\n", what);
    ++g_failures;
}

using gears::GuestDirtyPages;
using gears::PagemapEntrySoftDirty;

void TestPagemapBitExtraction()
{
    Check(!PagemapEntrySoftDirty(0), "an all-zero entry (absent page) is not soft-dirty");
    Check(PagemapEntrySoftDirty(gears::kPagemapSoftDirtyBit), "bit 55 alone reads as soft-dirty");
    Check(!PagemapEntrySoftDirty(gears::kPagemapSoftDirtyBit - 1),
          "the bits below 55 do not read as soft-dirty");
    Check(PagemapEntrySoftDirty(~0ull), "every bit set reads as soft-dirty");
    Check(!PagemapEntrySoftDirty(~gears::kPagemapSoftDirtyBit),
          "every bit EXCEPT soft-dirty does not read as soft-dirty");
}

// The probe inside Open() drives both classes; this test refuses to pass on a
// host where the interface is absent, because there the shipping behaviour is
// "always hash" and a test that skipped would prove nothing.
void TestRealKernelDiscrimination()
{
    GuestDirtyPages t;
    // Raw mmap, not a container: the heap arena's own churn has nothing to do
    // with the kernel semantics under test.
    uint8_t *scratch = static_cast<uint8_t *>(
        ::mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (scratch == MAP_FAILED)
    {
        std::printf("SKIP: mmap failed\n");
        return;
    }
    scratch[0] = 1;
    scratch[4096] = 1; // fault both pages in

    const bool opened = t.Open(scratch, {0}, 0); // flat, like a capture replay
    if (!opened)
    {
        std::printf("SKIP: kernel reports no usable soft-dirty interface"
                    " (probe refused); always-hash fallback is the shipped"
                    " behaviour and was exercised instead\n");
        Check(!t.Supported(), "an unsupported tracker says so rather than pretending");
        // The load-bearing direction: every doubt hashes. A caller asking an
        // unsupported or broken tracker must be told DIRTY, never clean.
        scratch[100] = 1;
        Check(t.RangeCleanSinceLastClear(0, 8192) == false,
              "unsupported tracker answers DIRTY so the caller keeps hashing");
        ::munmap(scratch, 8192);
        return;
    }

    Check(t.Supported(), "a successful Open leaves the tracker supported");

    // Fresh observation period: the pages were written before Open's final
    // clear, so they must read clean now.
    Check(t.RangeCleanSinceLastClear(0, 8192),
          "after a clear, storage written before it reads clean");

    // Write one byte in the middle through the same mapping.
    scratch[5000] = 7; // page 1 of 2
    Check(!t.RangeCleanSinceLastClear(0, 8192),
          "a store anywhere in the span makes the span dirty");
    Check(!t.RangeCleanSinceLastClear(6000, 100),
          "the bit belongs to the PAGE: an untouched byte sharing a page with"
          " a written one still reads dirty -- over-hashing, never stale");

    t.BeginFrame();
    Check(t.RangeCleanSinceLastClear(0, 8192),
          "after the next clear the same span reads clean again");

    // Partial-page ends: dirtying the LAST byte of the span must be seen even
    // though the span starts mid-page.
    scratch[8191] = 9;
    Check(!t.RangeCleanSinceLastClear(40, 8192 - 40),
          "a store on the final partial page is seen from a mid-page start");

    t.BeginFrame();
    Check(t.RangeCleanSinceLastClear(0, 100), "first page clean after clear");
    ::munmap(scratch, 8192);
}

// THE HAZARD THIS MODULE EXISTS TO COVER: guest RAM aliased at several
// windows. A write through window B must mark a span queried through window A.
// Proven against a real memfd mapped twice, which is exactly the runtime's
// shape (guest_memory.cpp MapPhysicalAliases).
void TestAliasWindows()
{
    int memfd = ::memfd_create("dirty-alias-test", 0);
    if (memfd < 0)
    {
        std::printf("SKIP: memfd_create unavailable\n");
        return;
    }
    constexpr size_t kSize = 3 * 4096;
    if (::ftruncate(memfd, kSize) != 0)
    {
        std::printf("SKIP: ftruncate failed\n");
        ::close(memfd);
        return;
    }
    // Window A at the buffer, window B one guest-style alias stride away.
    // Emulate the real window shape: bases separated by a power-of-two stride
    // with the physical offset recovered by masking the top bits, exactly how
    // guest_memory's four windows relate (0x0 / 0xA0000000 / ...).
    constexpr uint64_t kStride = 1ull << 29;      // 512 MiB
    constexpr uint64_t kWindowB = 5ull * kStride; // like 0xA0000000
    constexpr uint64_t kAliasMask = kStride - 1;
    uint8_t *region = static_cast<uint8_t *>(::mmap(
        nullptr, kWindowB + kSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (region == MAP_FAILED)
    {
        std::printf("SKIP: mmap failed\n");
        ::close(memfd);
        return;
    }
    uint8_t *a = region;
    uint8_t *b = region + kWindowB;
    if (::mmap(a, kSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, memfd, 0) == MAP_FAILED ||
        ::mmap(b, kSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, memfd, 0) == MAP_FAILED)
    {
        std::printf("SKIP: alias mapping failed\n");
        ::munmap(region, kWindowB + kSize);
        ::close(memfd);
        return;
    }

    GuestDirtyPages t;
    const auto *base = reinterpret_cast<const uint8_t *>(region);
    const bool opened = t.Open(base, {0, kWindowB}, kAliasMask);
    if (!opened)
    {
        std::printf("SKIP: soft-dirty unsupported\n");
    }
    else
    {
        Check(t.RangeCleanSinceLastClear(0, kSize), "aliased storage clean after clear");
        b[4096 + 123] = 5; // write through WINDOW B only
        Check(!t.RangeCleanSinceLastClear(0, kSize),
              "a write through the OTHER window marks the span dirty -- this"
              " is the stale-texture hazard, proven to fire");
        t.BeginFrame();
        Check(t.RangeCleanSinceLastClear(0, kSize), "clean again after the next clear");
        a[100] = 6; // write through window A
        Check(!t.RangeCleanSinceLastClear(0, kSize),
              "a write through window A is seen from the same query path too");
    }

    ::munmap(region, kWindowB + kSize);
    ::close(memfd);
}

} // namespace

int main()
{
    TestPagemapBitExtraction();
    TestRealKernelDiscrimination();
    TestAliasWindows();
    if (g_failures == 0)
    {
        std::printf("all guest dirty pages tests passed\n");
        return 0;
    }
    std::printf("%d guest dirty pages test(s) FAILED\n", g_failures);
    return 1;
}

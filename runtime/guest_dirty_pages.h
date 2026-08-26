#pragma once

// Which guest-memory pages has THIS process written since we last looked?
//
// WHY THIS EXISTS. The texture cache must notice when the guest overwrites
// pixels under an unchanged fetch constant (catalog #53: the startup movie
// froze at its first frame), and the only mechanism it had was re-reading and
// re-hashing every cached texture's storage every frame -- 49.39 MiB per
// chapter-45 frame, 3.9 ms, 12.54% of sampled CPU cycles in the hash itself
// (catalog #137). The information "did these bytes change" is one the kernel
// already maintains: the soft-dirty PTE bit, set by any store through any
// mapping, cleared only by an explicit request. Consulting it lets a texture
// whose pages are provably unwritten skip its hash without weakening the
// staleness guarantee.
//
// THE GUARANTEE, PRECISELY. Between a clean report for a span and the next
// clear, every store to that span through ANY configured window sets a bit the
// next query sees. The one hole is a store racing the clear itself (the clear
// wipes bits as it walks); a forced full re-hash every kTexDirtyVerifyFrames
// bounds whatever such a race could hide, and a miss found there -- bytes that
// changed while the tracker kept reporting them clean -- is counted, warned,
// and PERMANENTLY DISABLES skipping. A page-tracking answer is trusted exactly
// until reality contradicts it once.
//
// Every doubt hashes. An unsupported kernel, a failed read, a range this
// process never mapped: all report DIRTY (meaning "you must hash"), because
// the fallback direction of this primitive is today's behaviour, not silence.
// A caller that cannot open the interface therefore cannot ship stale
// textures; it can only ship the cost it always paid.

#include <cstdint>
#include <cstddef>
#include <vector>

namespace gears
{

// The soft-dirty bit's position in a /proc/self/pagemap entry. Exposed for
// tests; production code goes through GuestDirtyPages.
constexpr uint64_t kPagemapSoftDirtyBit = 1ull << 55;

inline bool PagemapEntrySoftDirty(uint64_t entry)
{
    return (entry & kPagemapSoftDirtyBit) != 0;
}

// Registers the process's LIVE guest-memory layout so staleness queries can
// recognise it and consult every alias window. Call once after the guest
// reservation exists (runtime/main.cpp). A process that never registers --
// a capture replay tool with its flat mirror -- gets single-window behaviour,
// which is exact there because nothing writes through another view.
void RegisterGuestAliasLayout(const void *base, std::vector<uint64_t> windows, uint64_t aliasMask);

// Fills `windows` with the offsets from `base` at which identical bytes can
// appear, and `aliasMask` with the physical-offset mask (0 when flat). False
// only for a null base.
bool StalenessWindowsFor(const void *base, std::vector<uint64_t> &windows, uint64_t &aliasMask);

// Every Nth observation period re-hashes every checked texture regardless of
// its page state. This bounds what the documented clear/write race could hide
// -- a store landing as the clear sweeps past it loses its bit -- to N frames,
// at a hashing cost of one full pass over the texture set divided by N. The
// verifications are also where a contradiction between the page answer and
// the bytes is detected and counted; see the staleness policy in
// gpu_draw_textures.cpp.
constexpr uint64_t kTexStalenessVerifyEvery = 64;

class GuestDirtyPages
{
  public: // Opens /proc/self/pagemap and /proc/self/clear_refs and PROVES both work
    // before claiming support: touches a private page, clears, requires the
    // page read back clean; touches it again, requires it read back dirty. A
    // tracker that could only ever answer one class would be worse than none,
    // so both answers are demanded here, on this kernel, at startup.
    //
    // `base` anchors every query. `windows` lists the offsets from base at
    // which identical bytes appear (the guest alias windows); `aliasMask` is
    // ANDed with a queried offset before adding each window, or zero for a
    // flat buffer with no aliasing, where `windows` should be {0}.
    bool Open(const uint8_t *base, std::vector<uint64_t> windows, uint64_t aliasMask);

    bool Supported() const { return fd_ >= 0 && clearFd_ >= 0 && probeOk_; }

    // Starts one observation period: bumps the generation, then clears every
    // soft-dirty bit in the process, so LATER queries answer "written since
    // this call". The texture policy calls this only AFTER consuming the
    // previous period. Clearing at the start of the next frame would erase the
    // inter-frame writes that the next query exists to detect.
    //
    // A failed clear counts against support: bits that survive would
    // over-report dirtiness forever, which is safe but useless, and repeated
    // failure means the interface is not working on this host.
    void BeginObservationPeriod();

    uint64_t Generation() const { return generation_; }

    // Whether NO byte of [offsetFromBase, +bytes) in ANY window was stored to
    // since the last BeginObservationPeriod. False -- hash -- on every doubt: unsupported,
    // short read, offset outside every mapping, zero-length span is true only
    // vacuously. Unmapped pages count clean: a page that was never faulted in
    // cannot have been stored to through this process.
    bool RangeCleanSinceLastClear(uint64_t offsetFromBase, size_t bytes) const;

    // Counters for the report line, all printed with their denominators:
    // spans/pages queried and how many answered dirty, plus clear failures.
    // Mutable because queries are const and these observe them.
    mutable uint64_t spansQueried = 0;
    mutable uint64_t pagesRead = 0;
    mutable uint64_t spansDirty = 0;
    mutable uint64_t preadShort = 0;
    uint64_t clearFailures = 0;

  private:
    int OpenPagemap();
    bool ReadEntries(uint64_t firstPage, size_t count, std::vector<uint64_t> &out) const;
    bool Probe();

    int fd_ = -1;
    int clearFd_ = -1;
    bool probeOk_ = false;
    const uint8_t *base_ = nullptr;
    std::vector<uint64_t> windows_;
    uint64_t aliasMask_ = 0;
    uint64_t generation_ = 0;
};

} // namespace gears

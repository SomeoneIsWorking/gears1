// A reference barrier: the port's replacement for a lifetime guarantee the
// console got from timing.
//
// The title's game thread holds a pointer to a pooled object across a call
// while its rendering thread destroys that object and returns the memory to the
// pool. Nothing in the guest image mediates that -- the engine has flags that
// would have been a legitimate seam and no code reads them -- so the race is
// real in the original and merely never lost on the hardware it shipped on. On
// this machine it is lost on roughly the 156th opportunity in every run.
//
// The barrier makes the freeing thread WAIT until no reader is inside the
// region holding that block. It never cancels or reorders a free: suppressing
// one leaks the block and desynchronises the pool's own bookkeeping, so the
// title's next allocation sees a pool that disagrees with itself. Delay is the
// weakest intervention that restores the guarantee.
#pragma once

#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <set>
#include <unordered_map>

namespace gears
{

class ReferenceBarrier
{
public:
    // Marks `address` as held by the calling thread until Leave() with the
    // returned ticket. Tickets rather than counts because the same thread
    // reaches the guarded region recursively, and a free must wait for the
    // OUTERMOST scope to close rather than the innermost.
    uint64_t Enter(uint32_t address);
    void Leave(uint32_t address, uint64_t ticket);

    // Blocks while any reader that was ALREADY inside when this call began
    // still holds `address`. Returns whether it had to wait at all.
    //
    // "Already inside" is deliberate. Waiting for the address to be unheld by
    // *anyone* lets a thread that enters and leaves the region in a loop starve
    // the freeing thread indefinitely, which converts a crash into a hang. The
    // guarantee the title needs is only that a reader holding the pointer when
    // the free begins still sees valid memory when it dereferences it.
    bool WaitUntilUnreferenced(uint32_t address,
                               std::chrono::milliseconds deadline);

    // How many frees actually had to wait, and how many gave up at the
    // deadline. Both are reported at shutdown: a run where the barrier never
    // engaged and a run where it is broken both end without a crash, and only
    // these numbers separate the evidence from the coincidence.
    uint64_t WaitCount() const;
    uint64_t TimeoutCount() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable released_;
    std::unordered_map<uint32_t, std::set<uint64_t>> held_;
    uint64_t nextTicket_ = 1;
    uint64_t waits_ = 0;
    uint64_t timeouts_ = 0;
};

// RAII for the reader side, so an early return out of the guarded region cannot
// leave a block permanently held -- which would stall the pool rather than
// protect it.
class ReaderScope
{
public:
    ReaderScope(ReferenceBarrier& barrier, uint32_t address)
        : barrier_(barrier), address_(address), ticket_(barrier.Enter(address))
    {
    }

    ~ReaderScope() { barrier_.Leave(address_, ticket_); }

    ReaderScope(const ReaderScope&) = delete;
    ReaderScope& operator=(const ReaderScope&) = delete;

private:
    ReferenceBarrier& barrier_;
    uint32_t address_;
    uint64_t ticket_;
};

// The process-wide barrier the guest hooks use.
ReferenceBarrier& ObjectBarrier();

} // namespace gears

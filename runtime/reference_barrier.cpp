#include "reference_barrier.h"

#include <algorithm>
#include <vector>

namespace gears
{

uint64_t ReferenceBarrier::Enter(uint32_t address)
{
    std::lock_guard<std::mutex> guard(mutex_);
    const uint64_t ticket = nextTicket_++;
    held_[address].insert(ticket);
    return ticket;
}

void ReferenceBarrier::Leave(uint32_t address, uint64_t ticket)
{
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto it = held_.find(address);
        if (it == held_.end())
            return;
        it->second.erase(ticket);
        if (it->second.empty())
            held_.erase(it);
    }
    // Notify outside the lock: the waiter's predicate takes it, and waking a
    // thread that immediately blocks on the mutex we still own is a needless
    // round trip on a path the renderer takes constantly.
    released_.notify_all();
}

bool ReferenceBarrier::WaitUntilUnreferenced(uint32_t address,
                                             std::chrono::milliseconds deadline)
{
    std::unique_lock<std::mutex> guard(mutex_);

    const auto it = held_.find(address);
    if (it == held_.end() || it->second.empty())
        return false;               // the overwhelmingly common case

    // Snapshot the readers that are inside RIGHT NOW. Tickets rise
    // monotonically, so "still one of mine" is a set membership test and a
    // reader that arrives later is invisible to this wait by construction --
    // that is what stops a churning reader from starving the free.
    const std::set<uint64_t> waitingOn = it->second;

    ++waits_;

    const auto stillHeld = [&] {
        const auto now = held_.find(address);
        if (now == held_.end())
            return false;
        return std::any_of(waitingOn.begin(), waitingOn.end(),
            [&](uint64_t ticket) { return now->second.count(ticket) != 0; });
    };

    if (!released_.wait_for(guard, deadline, [&] { return !stillHeld(); }))
    {
        // GIVING UP IS REPORTED, NEVER SWALLOWED. The free proceeds after this,
        // so the original race is back for this one block; a run where that
        // happened must not look identical to a run where the barrier held.
        ++timeouts_;
    }
    return true;
}

uint64_t ReferenceBarrier::WaitCount() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return waits_;
}

uint64_t ReferenceBarrier::TimeoutCount() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return timeouts_;
}

ReferenceBarrier& ObjectBarrier()
{
    static ReferenceBarrier barrier;
    return barrier;
}

} // namespace gears

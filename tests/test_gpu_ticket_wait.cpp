#include "gpu_ticket_wait.h"

#include <cassert>
#include <chrono>
#include <future>
#include <latch>

int main()
{
    using namespace std::chrono_literals;

    assert(gears::RemainingGpuTicketWait(100, 100) == 5'000ms);
    assert(gears::RemainingGpuTicketWait(4'999, 0) == 1ms);
    assert(gears::RemainingGpuTicketWait(5'000, 0) == 0ms);
    assert(gears::RemainingGpuTicketWait(3, 0xFFFF'FFFEu) == 4'995ms);

    gears::GpuPacketMemoryChangeTracker tracker;
    const gears::GpuPacketMemoryObservation observed = tracker.Observe(0x30A000);

    tracker.Notify(0x30B000);
    assert(!tracker.WaitUntil(observed, std::chrono::steady_clock::now()));

    tracker.Notify(0x30A000);
    assert(tracker.WaitUntil(observed, std::chrono::steady_clock::now()));

    const gears::GpuPacketMemoryObservation aliased = gears::ObserveGpuPacketMemory(0x20300003);
    gears::NotifyGpuPacketMemoryChanged(0x00300000);
    assert(gears::WaitForGpuPacketMemoryChange(aliased, std::chrono::steady_clock::now()));

    const gears::GpuPacketMemoryObservation blocking = tracker.Observe(0x30A000);
    std::latch ready(1);
    auto waiter =
        std::async(std::launch::async,
                   [&tracker, blocking, &ready]
                   {
                       ready.count_down();
                       return tracker.WaitUntil(blocking, std::chrono::steady_clock::now() + 1s);
                   });
    ready.wait();
    tracker.Notify(0x30A000);
    assert(waiter.get());

    return 0;
}

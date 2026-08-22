#include "gpu_queue_access.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>

int main()
{
    gears::GpuQueueAccess access;
    std::atomic<uint32_t> active{0};
    std::atomic<uint32_t> maximum{0};

    std::vector<std::thread> workers;
    constexpr uint32_t kWorkers = 8;
    constexpr uint32_t kOperations = 1000;
    workers.reserve(kWorkers);
    for (uint32_t worker = 0; worker < kWorkers; ++worker)
    {
        workers.emplace_back(
            [&]
            {
                for (uint32_t operation = 0; operation < kOperations; ++operation)
                {
                    access.Invoke(
                        [&]
                        {
                            const uint32_t now = active.fetch_add(1) + 1;
                            uint32_t observed = maximum.load();
                            while (observed < now && !maximum.compare_exchange_weak(observed, now))
                            {
                            }
                            std::this_thread::yield();
                            active.fetch_sub(1);
                        });
                }
            });
    }
    for (std::thread &worker : workers)
        worker.join();

    assert(active.load() == 0);
    assert(maximum.load() == 1);
}

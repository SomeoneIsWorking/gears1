#include "gpu_retirement.h"

#include <cassert>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace
{

class FakeCompletionBackend final : public gears::GpuCompletionBackend
{
  public:
    Status Poll(gears::GpuCompletionPoint point) override
    {
        ++pollCalls;
        return statuses[point.value];
    }

    bool Wait(gears::GpuCompletionPoint point) override
    {
        ++waitCalls;
        if (statuses[point.value] == Status::Error)
            return false;
        statuses[point.value] = Status::Complete;
        return true;
    }

    std::unordered_map<uint64_t, Status> statuses;
    uint32_t pollCalls = 0;
    uint32_t waitCalls = 0;
};

} // namespace

int main()
{
    using gears::GpuCompletionBackend;
    using gears::GpuCompletionPoint;
    using gears::GpuRetirement;

    FakeCompletionBackend backend;
    GpuRetirement retirement(backend, 2);
    std::vector<uint32_t> completed;

    auto first = retirement.Acquire();
    auto second = retirement.Acquire();
    assert(first.has_value() && second.has_value());
    assert(first->slot != second->slot);
    assert(retirement.RecordingCount() == 2);
    assert(!retirement.Drain());

    backend.statuses[10] = GpuCompletionBackend::Status::Pending;
    backend.statuses[20] = GpuCompletionBackend::Status::Pending;
    assert(retirement.Submit(*first, GpuCompletionPoint{10}, [&] { completed.push_back(10); }));
    assert(retirement.Submit(*second, GpuCompletionPoint{20}, [&] { completed.push_back(20); }));
    assert(retirement.InFlightCount() == 2);

    // A full owner polls but never waits or discards an in-flight resource.
    assert(!retirement.Acquire().has_value());
    assert(backend.pollCalls == 2);
    assert(backend.waitCalls == 0);
    assert(completed.empty());

    // Independent fences may retire out of order; only the completed slot is
    // recycled, and an old lease cannot cancel its new generation.
    backend.statuses[20] = GpuCompletionBackend::Status::Complete;
    assert(retirement.Poll());
    assert((completed == std::vector<uint32_t>{20}));
    auto recycled = retirement.Acquire();
    assert(recycled.has_value() && recycled->slot == second->slot);
    assert(!retirement.Cancel(*second));
    assert(retirement.Cancel(*recycled));

    backend.statuses[10] = GpuCompletionBackend::Status::Complete;
    assert(retirement.Poll());
    assert((completed == std::vector<uint32_t>{20, 10}));
    assert(retirement.InFlightCount() == 0);

    // Explicit drain is the only normal blocking path and completes cleanup.
    auto draining = retirement.Acquire();
    assert(draining.has_value());
    backend.statuses[30] = GpuCompletionBackend::Status::Pending;
    assert(retirement.Submit(*draining, GpuCompletionPoint{30}, [&] { completed.push_back(30); }));
    assert(retirement.Drain());
    assert(backend.waitCalls == 1);
    assert((completed == std::vector<uint32_t>{20, 10, 30}));

    // An error is not completion: the slot and callback remain owned until the
    // caller explicitly enters the device-loss release path.
    auto lost = retirement.Acquire();
    assert(lost.has_value());
    backend.statuses[40] = GpuCompletionBackend::Status::Error;
    assert(retirement.Submit(*lost, GpuCompletionPoint{40}, [&] { completed.push_back(40); }));
    assert(!retirement.Poll());
    assert(!retirement.Healthy());
    assert(retirement.InFlightCount() == 1);
    assert(!retirement.Acquire().has_value());
    assert(!retirement.Drain());
    assert(completed.back() == 30);
    assert(retirement.ReleaseAfterBackendLoss());
    assert(retirement.InFlightCount() == 0);
    assert(completed.back() == 40);
}

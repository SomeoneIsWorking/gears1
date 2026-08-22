#include "gpu_retirement.h"

#include <cstdlib>
#include <utility>

namespace gears
{

GpuRetirement::GpuRetirement(GpuCompletionBackend &backend, size_t capacity)
    : backend_(backend), slots_(capacity)
{
    if (capacity == 0)
        std::abort();
}

GpuRetirement::~GpuRetirement()
{
    if (RecordingCount() != 0 || InFlightCount() != 0)
        std::abort();
}

std::optional<GpuRetirement::Lease> GpuRetirement::Acquire()
{
    if (!Poll())
        return std::nullopt;
    for (size_t index = 0; index < slots_.size(); ++index)
    {
        Slot &slot = slots_[index];
        if (slot.state != SlotState::Free)
            continue;
        ++slot.generation;
        if (slot.generation == 0)
            std::abort();
        slot.state = SlotState::Recording;
        return Lease{index, slot.generation};
    }
    return std::nullopt;
}

bool GpuRetirement::Submit(Lease lease, GpuCompletionPoint point, Completion completion)
{
    if (!healthy_ || !Matches(lease, SlotState::Recording))
        return false;
    Slot &slot = slots_[lease.slot];
    slot.point = point;
    slot.completion = std::move(completion);
    slot.state = SlotState::InFlight;
    return true;
}

bool GpuRetirement::Cancel(Lease lease)
{
    if (!Matches(lease, SlotState::Recording))
        return false;
    slots_[lease.slot].state = SlotState::Free;
    return true;
}

bool GpuRetirement::Poll()
{
    if (!healthy_)
        return false;

    std::vector<Completion> ready;
    for (Slot &slot : slots_)
    {
        if (slot.state != SlotState::InFlight)
            continue;
        const GpuCompletionBackend::Status status = backend_.Poll(slot.point);
        if (status == GpuCompletionBackend::Status::Error)
        {
            healthy_ = false;
            break;
        }
        if (status == GpuCompletionBackend::Status::Complete)
            Finish(slot, ready);
    }
    Run(ready);
    return healthy_;
}

bool GpuRetirement::Drain()
{
    if (!healthy_ || RecordingCount() != 0)
        return false;

    std::vector<Completion> ready;
    for (Slot &slot : slots_)
    {
        if (slot.state != SlotState::InFlight)
            continue;
        if (!backend_.Wait(slot.point))
        {
            healthy_ = false;
            Run(ready);
            return false;
        }
        Finish(slot, ready);
    }
    Run(ready);
    return true;
}

bool GpuRetirement::ReleaseAfterBackendLoss()
{
    if (healthy_ || RecordingCount() != 0)
        return false;
    std::vector<Completion> ready;
    for (Slot &slot : slots_)
    {
        if (slot.state == SlotState::InFlight)
            Finish(slot, ready);
    }
    Run(ready);
    return true;
}

size_t GpuRetirement::RecordingCount() const
{
    size_t count = 0;
    for (const Slot &slot : slots_)
        count += slot.state == SlotState::Recording ? 1 : 0;
    return count;
}

size_t GpuRetirement::InFlightCount() const
{
    size_t count = 0;
    for (const Slot &slot : slots_)
        count += slot.state == SlotState::InFlight ? 1 : 0;
    return count;
}

bool GpuRetirement::Matches(const Lease &lease, SlotState expected) const
{
    return lease.slot < slots_.size() && slots_[lease.slot].generation == lease.generation &&
           slots_[lease.slot].state == expected;
}

void GpuRetirement::Finish(Slot &slot, std::vector<Completion> &ready)
{
    if (slot.completion)
        ready.push_back(std::move(slot.completion));
    slot.point = {};
    slot.state = SlotState::Free;
}

void GpuRetirement::Run(std::vector<Completion> &ready)
{
    for (Completion &completion : ready)
        completion();
}

} // namespace gears

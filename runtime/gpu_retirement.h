#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace gears
{

// A backend-owned completion point. A Vulkan adapter may map this to a timeline
// semaphore value or to an ID for a fence stored beside a frame resource slot.
struct GpuCompletionPoint
{
    uint64_t value = 0;
};

class GpuCompletionBackend
{
  public:
    enum class Status
    {
        Pending,
        Complete,
        Error,
    };

    virtual ~GpuCompletionBackend() = default;
    virtual Status Poll(GpuCompletionPoint point) = 0;
    virtual bool Wait(GpuCompletionPoint point) = 0;
};

// Owns a bounded set of frame-resource lifetimes. Normal progress only polls;
// Wait is reserved for explicit drain at teardown. Calls are serialized by the
// renderer owner, and completions run only after their point has retired.
class GpuRetirement
{
  public:
    using Completion = std::function<void()>;

    struct Lease
    {
        size_t slot = 0;
        uint64_t generation = 0;
    };

    GpuRetirement(GpuCompletionBackend &backend, size_t capacity);
    ~GpuRetirement();

    GpuRetirement(const GpuRetirement &) = delete;
    GpuRetirement &operator=(const GpuRetirement &) = delete;

    std::optional<Lease> Acquire();
    bool Submit(Lease lease, GpuCompletionPoint point, Completion completion);
    bool Cancel(Lease lease);

    // Polls every in-flight point once. An errored point remains owned and is
    // never reported as retired.
    bool Poll();

    // Explicit teardown barrier. Refuses while a recording lease exists.
    bool Drain();

    // Device loss ends GPU access by definition. This is the only path that
    // releases errored points without observing completion, and it runs their
    // cleanup callbacks rather than dropping resource ownership silently.
    bool ReleaseAfterBackendLoss();

    bool Healthy() const { return healthy_; }
    size_t Capacity() const { return slots_.size(); }
    size_t RecordingCount() const;
    size_t InFlightCount() const;

  private:
    enum class SlotState
    {
        Free,
        Recording,
        InFlight,
    };

    struct Slot
    {
        SlotState state = SlotState::Free;
        uint64_t generation = 0;
        GpuCompletionPoint point{};
        Completion completion;
    };

    bool Matches(const Lease &lease, SlotState expected) const;
    static void Finish(Slot &slot, std::vector<Completion> &ready);
    static void Run(std::vector<Completion> &ready);

    GpuCompletionBackend &backend_;
    std::vector<Slot> slots_;
    bool healthy_ = true;
};

} // namespace gears

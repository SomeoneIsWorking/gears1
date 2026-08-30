#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>

#include "gpu_draw.h"

namespace gears
{

enum class FrameQueueSubmitStatus
{
    Accepted,
    ReplacedPending,
    RejectedStale,
    RejectedClosed,
};

struct FrameQueueSubmitResult
{
    FrameQueueSubmitStatus status = FrameQueueSubmitStatus::RejectedClosed;
    std::optional<uint64_t> displacedFrameId;
    std::optional<uint64_t> displacedFrameSequence;

    [[nodiscard]] bool accepted() const
    {
        return status == FrameQueueSubmitStatus::Accepted ||
               status == FrameQueueSubmitStatus::ReplacedPending;
    }
};

struct QueuedFrame
{
    uint64_t frameId = 0;
    uint64_t retirementGeneration = 0;
    FrameDrawInputs inputs;
};

// A single-consumer, bounded latest-frame handoff. The producer never waits for
// renderer progress or queue capacity: while one frame is active, a newer
// submission replaces the sole pending frame. FrameDrawInputs ownership moves
// into the queue on acceptance, then into the consumer returned by WaitTake().
// Its raw guest-memory pointers remain borrowed and retain the lifetime contract
// documented by gpu_draw.h.
class FrameQueue final
{
  public:
    FrameQueue() = default;
    FrameQueue(const FrameQueue &) = delete;
    FrameQueue &operator=(const FrameQueue &) = delete;

    FrameQueueSubmitResult Submit(uint64_t frameId, uint64_t retirementGeneration,
                                  FrameDrawInputs &&inputs);

    // Blocks only the consumer. After Close(), drains the accepted latest frame
    // and then returns nullopt deterministically.
    std::optional<QueuedFrame> WaitTake();

    // Completes only the active frame. A mismatched ID is rejected without
    // changing queue state, making stale completion attempts harmless.
    bool Complete(uint64_t frameId);

    // Refuses future submissions but preserves the pending frame for draining.
    // Idempotent and safe whether the consumer is waiting or rendering.
    void Close();

    // Intended for capture and shutdown control paths, never the producer's
    // live submission path.
    void WaitIdle();

  private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<QueuedFrame> pending_;
    std::optional<uint64_t> activeFrameId_;
    std::optional<uint64_t> lastSubmittedFrameId_;
    bool closed_ = false;
};

} // namespace gears

#include "frame_queue.h"

#include <utility>

namespace gears
{

FrameQueueSubmitResult FrameQueue::Submit(uint64_t frameId, uint64_t retirementGeneration,
                                          FrameDrawInputs &&inputs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_)
        return {FrameQueueSubmitStatus::RejectedClosed, std::nullopt};
    if (lastSubmittedFrameId_.has_value() && frameId <= *lastSubmittedFrameId_)
        return {FrameQueueSubmitStatus::RejectedStale, std::nullopt};

    lastSubmittedFrameId_ = frameId;
    const std::optional<uint64_t> displacedFrameId =
        pending_.has_value() ? std::optional<uint64_t>(pending_->frameId) : std::nullopt;
    pending_.emplace(QueuedFrame{frameId, retirementGeneration, std::move(inputs)});
    changed_.notify_one();
    return {displacedFrameId.has_value() ? FrameQueueSubmitStatus::ReplacedPending
                                         : FrameQueueSubmitStatus::Accepted,
            displacedFrameId};
}

std::optional<QueuedFrame> FrameQueue::WaitTake()
{
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock,
                  [this]
                  {
                      return (!activeFrameId_.has_value() && pending_.has_value()) ||
                             (closed_ && !activeFrameId_.has_value());
                  });
    if (!pending_.has_value())
        return std::nullopt;

    std::optional<QueuedFrame> frame(std::move(pending_));
    pending_.reset();
    activeFrameId_ = frame->frameId;
    return frame;
}

bool FrameQueue::Complete(uint64_t frameId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!activeFrameId_.has_value() || *activeFrameId_ != frameId)
        return false;

    activeFrameId_.reset();
    changed_.notify_all();
    return true;
}

void FrameQueue::Close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    changed_.notify_all();
}

void FrameQueue::WaitIdle()
{
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [this] { return !activeFrameId_.has_value() && !pending_.has_value(); });
}

} // namespace gears

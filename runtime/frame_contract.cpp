#include "frame_contract.h"

namespace gears
{

FrameTransition FrameContract::Publish(FrameId frame)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!frame)
        return FrameTransition::kRejectedInvalid;
    if (frame == published_)
        return FrameTransition::kRejectedDuplicate;
    if (frame < published_)
        return FrameTransition::kRejectedRegression;
    published_ = frame;
    return FrameTransition::kAdvanced;
}

FrameTransition FrameContract::Present(FrameId frame)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!frame)
        return FrameTransition::kRejectedInvalid;
    if (!published_ || frame > published_)
        return FrameTransition::kRejectedUnpublished;
    if (frame < published_)
        return FrameTransition::kRejectedStale;
    if (frame == presented_)
        return FrameTransition::kRepeated;
    presented_ = frame;
    return FrameTransition::kAdvanced;
}

FrameContractSnapshot FrameContract::Snapshot() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return {published_, presented_};
}

} // namespace gears

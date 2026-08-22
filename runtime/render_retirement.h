#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace gears
{

// Tracks the render generation an asynchronous GPU completion belongs to.
// The owner serializes calls; this class owns only the ordering rule so the
// production transition can be tested without starting Vulkan or a thread.
class RenderRetirement
{
  public:
    using Completion = std::function<void()>;

    struct FinishBatch
    {
        std::vector<Completion> completions;
        bool generationComplete = false;
    };

    uint64_t AcceptFrame() { return ++lastAcceptedGeneration_; }

    // Returns true when queued. A false result leaves completion untouched so
    // the caller can run it after releasing its lock.
    bool Defer(Completion &completion)
    {
        if (lastAcceptedGeneration_ <= completedGeneration_)
            return false;
        completions_.push_back(PendingCompletion{lastAcceptedGeneration_, std::move(completion)});
        return true;
    }

    // Called repeatedly after rendering generation. Completions run outside
    // the owner's lock; a completion queued concurrently for the same
    // generation is returned by the next call before the generation closes.
    FinishBatch Finish(uint64_t generation)
    {
        size_t readyCount = 0;
        // The command processor is the single producer, so target generations
        // are monotonic and every ready completion is a prefix.
        while (readyCount < completions_.size() &&
               completions_[readyCount].generation <= generation)
            ++readyCount;
        if (readyCount == 0)
        {
            completedGeneration_ = generation;
            return FinishBatch{{}, true};
        }

        FinishBatch batch;
        batch.completions.reserve(readyCount);
        for (size_t i = 0; i < readyCount; ++i)
            batch.completions.push_back(std::move(completions_[i].run));
        completions_.erase(completions_.begin(),
                           completions_.begin() + static_cast<std::ptrdiff_t>(readyCount));
        return batch;
    }

  private:
    struct PendingCompletion
    {
        uint64_t generation = 0;
        Completion run;
    };

    uint64_t lastAcceptedGeneration_ = 0;
    uint64_t completedGeneration_ = 0;
    std::vector<PendingCompletion> completions_;
};

} // namespace gears

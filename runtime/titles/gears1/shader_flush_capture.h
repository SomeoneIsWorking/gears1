#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace gears::titles::gears1
{

struct ShaderCommandRange
{
    std::uint32_t commandBefore = 0;
    std::uint32_t commandEnd = 0;
};

struct ShaderFlushCaptureResult
{
    bool complete = false;
    std::vector<ShaderCommandRange> ranges;
};

class ShaderFlushRangeCapture
{
  public:
    [[nodiscard]] bool Active() const { return active_; }

    [[nodiscard]] bool Begin(std::uint32_t device, std::uint32_t commandBefore)
    {
        if (active_ || device == 0 || (commandBefore & 3) != 0)
            return false;
        active_ = true;
        complete_ = true;
        device_ = device;
        commandBefore_ = commandBefore;
        ranges_.clear();
        return true;
    }

    void ObserveCommandBufferTransition(std::uint32_t device, std::uint32_t oldCommandEnd,
                                        std::uint32_t newCommandBefore)
    {
        if (!active_)
            return;
        if (device != device_ || (newCommandBefore & 3) != 0)
        {
            complete_ = false;
            return;
        }
        AppendRange(oldCommandEnd);
        commandBefore_ = newCommandBefore;
    }

    [[nodiscard]] ShaderFlushCaptureResult Finish(std::uint32_t device, std::uint32_t commandEnd)
    {
        if (!active_)
            return {};
        if (device != device_)
            complete_ = false;
        else
            AppendRange(commandEnd);

        ShaderFlushCaptureResult result{
            .complete = complete_,
            .ranges = std::move(ranges_),
        };
        active_ = false;
        complete_ = false;
        device_ = 0;
        commandBefore_ = 0;
        ranges_.clear();
        return result;
    }

  private:
    void AppendRange(std::uint32_t commandEnd)
    {
        if ((commandEnd & 3) != 0 || commandEnd < commandBefore_)
        {
            complete_ = false;
            return;
        }
        if (commandEnd != commandBefore_)
            ranges_.push_back({.commandBefore = commandBefore_, .commandEnd = commandEnd});
    }

    bool active_ = false;
    bool complete_ = false;
    std::uint32_t device_ = 0;
    std::uint32_t commandBefore_ = 0;
    std::vector<ShaderCommandRange> ranges_;
};

// Called by the retained command-buffer rollover seam while a shader flush is
// active on the same guest thread.
void ObserveShaderFlushCommandBufferTransition(std::uint32_t device, std::uint32_t oldCommandEnd,
                                               std::uint32_t newCommandBefore);
void ObserveShaderCommandBufferSubmission(std::uint32_t device, std::uint32_t guestAddress,
                                          std::uint32_t dwordCount);
[[nodiscard]] bool ShaderFlushCaptureActive();

} // namespace gears::titles::gears1

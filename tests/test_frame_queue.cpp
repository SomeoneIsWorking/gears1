#include "frame_queue.h"
#include "render_retirement.h"

#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace
{

gears::FrameDrawInputs MakeFrame(long sequence)
{
    gears::FrameDrawInputs inputs;
    inputs.sequence = sequence;
    return inputs;
}

} // namespace

int main()
{
    {
        gears::FrameQueue queue;
        gears::FrameDrawInputs first = MakeFrame(10);
        const gears::FrameQueueSubmitResult submitted = queue.Submit(0, 7, std::move(first));
        assert(submitted.status == gears::FrameQueueSubmitStatus::Accepted);
        assert(submitted.accepted());
        assert(!submitted.displacedFrameId.has_value());

        std::optional<gears::QueuedFrame> active = queue.WaitTake();
        assert(active.has_value());
        assert(active->frameId == 0);
        assert(active->retirementGeneration == 7);
        assert(active->inputs.sequence == 10);
        assert(!queue.Complete(1));
        assert(queue.Complete(0));
        queue.WaitIdle();
    }

    {
        gears::FrameQueue queue;
        auto oldSnapshot = std::make_shared<const std::vector<uint32_t>>(1, 42);
        std::weak_ptr<const std::vector<uint32_t>> oldLifetime = oldSnapshot;
        gears::FrameDrawInputs oldFrame = MakeFrame(20);
        oldFrame.draws.push_back(gears::FrameDrawItem{.registerFile = std::move(oldSnapshot)});
        assert(queue.Submit(4, 11, std::move(oldFrame)).accepted());

        gears::FrameDrawInputs latestFrame = MakeFrame(21);
        const gears::FrameQueueSubmitResult replaced = queue.Submit(5, 12, std::move(latestFrame));
        assert(replaced.status == gears::FrameQueueSubmitStatus::ReplacedPending);
        assert(replaced.accepted());
        assert(replaced.displacedFrameId == 4);
        assert(oldLifetime.expired());

        auto staleSnapshot = std::make_shared<const std::vector<uint32_t>>(1, 43);
        std::weak_ptr<const std::vector<uint32_t>> staleLifetime = staleSnapshot;
        gears::FrameDrawInputs staleFrame = MakeFrame(19);
        staleFrame.draws.push_back(gears::FrameDrawItem{.registerFile = std::move(staleSnapshot)});
        const gears::FrameQueueSubmitResult stale = queue.Submit(5, 13, std::move(staleFrame));
        assert(stale.status == gears::FrameQueueSubmitStatus::RejectedStale);
        assert(!stale.accepted());
        assert(!staleLifetime.expired());

        std::optional<gears::QueuedFrame> active = queue.WaitTake();
        assert(active.has_value());
        assert(active->frameId == 5);
        assert(active->retirementGeneration == 12);
        assert(active->inputs.sequence == 21);
        assert(queue.Complete(5));
    }

    {
        gears::FrameQueue queue;
        gears::RenderRetirement retirement;
        std::vector<uint32_t> completionOrder;

        const uint64_t displacedGeneration = retirement.AcceptFrame();
        assert(queue.Submit(6, displacedGeneration, MakeFrame(22)).accepted());
        std::function<void()> displacedDone = [&completionOrder] { completionOrder.push_back(6); };
        assert(retirement.Defer(displacedDone));

        const uint64_t latestGeneration = retirement.AcceptFrame();
        assert(queue.Submit(7, latestGeneration, MakeFrame(23)).accepted());
        std::function<void()> latestDone = [&completionOrder] { completionOrder.push_back(7); };
        assert(retirement.Defer(latestDone));

        std::optional<gears::QueuedFrame> latest = queue.WaitTake();
        assert(latest.has_value());
        assert(latest->frameId == 7);
        assert(latest->retirementGeneration == latestGeneration);
        for (;;)
        {
            gears::RenderRetirement::FinishBatch batch =
                retirement.Finish(latest->retirementGeneration);
            for (gears::RenderRetirement::Completion &completion : batch.completions)
                completion();
            if (batch.generationComplete)
                break;
        }
        assert((completionOrder == std::vector<uint32_t>{6, 7}));
        assert(queue.Complete(7));
    }

    {
        gears::FrameQueue queue;
        assert(queue.Submit(8, 14, MakeFrame(30)).accepted());
        queue.Close();

        auto rejectedSnapshot = std::make_shared<const std::vector<uint32_t>>(1, 44);
        std::weak_ptr<const std::vector<uint32_t>> rejectedLifetime = rejectedSnapshot;
        gears::FrameDrawInputs rejectedFrame = MakeFrame(31);
        rejectedFrame.draws.push_back(
            gears::FrameDrawItem{.registerFile = std::move(rejectedSnapshot)});
        const gears::FrameQueueSubmitResult closed = queue.Submit(9, 15, std::move(rejectedFrame));
        assert(closed.status == gears::FrameQueueSubmitStatus::RejectedClosed);
        assert(!closed.accepted());
        assert(!rejectedLifetime.expired());

        std::optional<gears::QueuedFrame> drained = queue.WaitTake();
        assert(drained.has_value());
        assert(drained->frameId == 8);
        assert(drained->inputs.sequence == 30);
        assert(queue.Complete(8));
        assert(!queue.WaitTake().has_value());
        queue.WaitIdle();
    }

    {
        gears::FrameQueue queue;
        std::optional<gears::QueuedFrame> result;
        std::thread consumer([&queue, &result] { result = queue.WaitTake(); });
        queue.Close();
        consumer.join();
        assert(!result.has_value());
    }

    return 0;
}

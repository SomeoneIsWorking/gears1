#include "gpu_frame_slots.h"

#include "gpu_frame_timing.h"
#include "gpu_retirement.h"

#include <condition_variable>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace gears::draw
{
namespace
{

void ReleaseFrameResources(VkDevice device, GpuFrameResources &frame)
{
    if (frame.guestMemoryMapped)
        vkUnmapMemory(device, frame.guestMemoryAllocation);
    vkDestroyBuffer(device, frame.guestMemory, nullptr);
    vkFreeMemory(device, frame.guestMemoryAllocation, nullptr);

    if (frame.arenaMapped)
        vkUnmapMemory(device, frame.arenaMemory);
    vkDestroyBuffer(device, frame.arena, nullptr);
    vkFreeMemory(device, frame.arenaMemory, nullptr);

    if (frame.readbackMapped)
        vkUnmapMemory(device, frame.readbackMemory);
    vkDestroyBuffer(device, frame.readback, nullptr);
    vkFreeMemory(device, frame.readbackMemory, nullptr);

    vkDestroyDescriptorPool(device, frame.depthAliasDescriptors, nullptr);
    vkDestroyDescriptorPool(device, frame.reinterpretDescriptors, nullptr);
    vkDestroyDescriptorPool(device, frame.resolveDescriptors, nullptr);
    vkDestroyDescriptorPool(device, frame.drawDescriptors, nullptr);
    vkDestroyCommandPool(device, frame.commandPool, nullptr);
    vkDestroyFence(device, frame.fence, nullptr);
    frame = {};
}

} // namespace

struct GpuFrameSlots::Impl
{
    struct FenceBackend final : GpuCompletionBackend
    {
        explicit FenceBackend(Impl &owner) : owner(owner) {}

        Status Poll(GpuCompletionPoint point) override
        {
            if (owner.backendLost || point.value == 0 || point.value > owner.frames.size())
                return Status::Error;
            const VkResult result =
                vkGetFenceStatus(owner.device, owner.frames[size_t(point.value - 1)].fence);
            if (result == VK_SUCCESS)
                return Status::Complete;
            return result == VK_NOT_READY ? Status::Pending : Status::Error;
        }

        bool Wait(GpuCompletionPoint point) override
        {
            if (owner.backendLost || point.value == 0 || point.value > owner.frames.size())
                return false;
            const VkFence fence = owner.frames[size_t(point.value - 1)].fence;
            return vkWaitForFences(owner.device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
        }

        Impl &owner;
    };

    struct OrderedCompletion
    {
        uint64_t sequence = 0;
        Completion run;
    };

    Impl(VkDevice inDevice, size_t capacity)
        : device(inDevice), frames(capacity), inFlight(capacity, false), backend(*this),
          retirement(backend, capacity)
    {
    }

    ~Impl()
    {
        if (!released)
            std::abort();
    }

    std::vector<Completion> CollectReadyLocked()
    {
        std::vector<Completion> ready;
        for (;;)
        {
            auto found = ordered.find(nextCompletionSequence);
            if (found == ordered.end())
                break;
            ready.push_back(std::move(found->second));
            ordered.erase(found);
            ++nextCompletionSequence;
        }
        return ready;
    }

    static void Run(std::vector<Completion> &ready, bool success)
    {
        for (Completion &completion : ready)
            completion(success);
    }

    void Pump()
    {
        for (;;)
        {
            std::vector<VkFence> fences;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&] { return stopping || retirement.InFlightCount() != 0; });
                if (stopping && retirement.InFlightCount() == 0)
                    return;
                for (size_t i = 0; i < frames.size(); ++i)
                    if (inFlight[i])
                        fences.push_back(frames[i].fence);
            }

            const VkResult waited = vkWaitForFences(device, uint32_t(fences.size()), fences.data(),
                                                    VK_FALSE, UINT64_MAX);

            std::vector<Completion> ready;
            bool success = waited == VK_SUCCESS;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!success)
                    backendLost = true;
                if (!retirement.Poll())
                {
                    success = false;
                    backendLost = true;
                    retirement.ReleaseAfterBackendLoss();
                }
                ready = CollectReadyLocked();
                callbacksRunning += ready.size();
                changed.notify_all();
            }
            Run(ready, success);
            {
                std::lock_guard<std::mutex> lock(mutex);
                callbacksRunning -= ready.size();
                changed.notify_all();
            }
        }
    }

    VkDevice device = VK_NULL_HANDLE;
    std::vector<GpuFrameResources> frames;
    std::vector<bool> inFlight;
    FenceBackend backend;
    GpuRetirement retirement;
    GpuFrameTiming timing;

    mutable std::mutex mutex;
    std::condition_variable changed;
    std::thread pump;
    std::map<uint64_t, Completion> ordered;
    uint64_t nextSubmitSequence = 1;
    uint64_t nextCompletionSequence = 1;
    size_t callbacksRunning = 0;
    bool backendLost = false;
    bool stopping = false;
    bool released = false;
};

GpuFrameSlots::GpuFrameSlots() = default;

GpuFrameSlots::~GpuFrameSlots()
{
    if (impl_)
        std::abort();
}

bool GpuFrameSlots::Initialize(VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily,
                               size_t capacity)
{
    if (impl_ || physical == VK_NULL_HANDLE || device == VK_NULL_HANDLE || capacity == 0)
        return false;
    std::unique_ptr<Impl> impl = std::make_unique<Impl>(device, capacity);
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (GpuFrameResources &frame : impl->frames)
        if (vkCreateFence(device, &fenceInfo, nullptr, &frame.fence) != VK_SUCCESS)
        {
            for (GpuFrameResources &created : impl->frames)
                ReleaseFrameResources(device, created);
            impl->released = true;
            return false;
        }
    impl->timing.Initialize(physical, device, queueFamily, capacity);
    impl->pump = std::thread([owner = impl.get()] { owner->Pump(); });
    impl_ = impl.release();
    return true;
}

void GpuFrameSlots::BeginTiming(const Lease &lease, VkCommandBuffer commands)
{
    if (impl_ && lease.resources && lease.slot < impl_->frames.size() &&
        lease.resources == &impl_->frames[lease.slot])
        impl_->timing.Begin(commands, lease.slot);
}

void GpuFrameSlots::EndTiming(const Lease &lease, VkCommandBuffer commands)
{
    if (impl_ && lease.resources && lease.slot < impl_->frames.size() &&
        lease.resources == &impl_->frames[lease.slot])
        impl_->timing.End(commands, lease.slot);
}

std::optional<GpuFrameSlots::Lease> GpuFrameSlots::Acquire()
{
    if (!impl_)
        return std::nullopt;
    for (;;)
    {
        std::vector<Completion> ready;
        std::optional<GpuRetirement::Lease> lease;
        {
            std::unique_lock<std::mutex> lock(impl_->mutex);
            if (impl_->stopping || impl_->backendLost)
                return std::nullopt;
            lease = impl_->retirement.Acquire();
            if (!impl_->retirement.Healthy())
            {
                impl_->backendLost = true;
                impl_->retirement.ReleaseAfterBackendLoss();
            }
            ready = impl_->CollectReadyLocked();
            impl_->callbacksRunning += ready.size();
            if (impl_->backendLost)
            {
                lock.unlock();
                Impl::Run(ready, false);
                lock.lock();
                impl_->callbacksRunning -= ready.size();
                impl_->changed.notify_all();
                return std::nullopt;
            }
            if (!lease)
            {
                lock.unlock();
                Impl::Run(ready, true);
                lock.lock();
                impl_->callbacksRunning -= ready.size();
                impl_->changed.notify_all();
                impl_->changed.wait(lock,
                                    [&]
                                    {
                                        return impl_->stopping || impl_->backendLost ||
                                               impl_->retirement.InFlightCount() <
                                                   impl_->retirement.Capacity();
                                    });
                continue;
            }
        }
        Impl::Run(ready, true);
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->callbacksRunning -= ready.size();
            impl_->changed.notify_all();
        }
        GpuFrameResources &frame = impl_->frames[lease->slot];
        if (vkResetFences(impl_->device, 1, &frame.fence) != VK_SUCCESS)
        {
            Cancel({lease->slot, lease->generation, &frame});
            return std::nullopt;
        }
        return Lease{lease->slot, lease->generation, &frame};
    }
}

bool GpuFrameSlots::Submit(Lease lease, Completion completion)
{
    if (!impl_ || !lease.resources || lease.slot >= impl_->frames.size() ||
        lease.resources != &impl_->frames[lease.slot] || !completion)
        return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const uint64_t sequence = impl_->nextSubmitSequence;
    const bool submitted = impl_->retirement.Submit(
        {lease.slot, lease.generation}, GpuCompletionPoint{lease.slot + 1},
        [owner = impl_, slot = lease.slot, sequence, completion = std::move(completion)]() mutable
        {
            owner->timing.Complete(slot, !owner->backendLost);
            owner->inFlight[slot] = false;
            owner->ordered.emplace(sequence, std::move(completion));
            owner->changed.notify_all();
        });
    if (submitted)
    {
        ++impl_->nextSubmitSequence;
        impl_->inFlight[lease.slot] = true;
        impl_->changed.notify_all();
    }
    return submitted;
}

bool GpuFrameSlots::Cancel(Lease lease)
{
    if (!impl_ || !lease.resources || lease.slot >= impl_->frames.size() ||
        lease.resources != &impl_->frames[lease.slot])
        return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const bool cancelled = impl_->retirement.Cancel({lease.slot, lease.generation});
    if (cancelled)
        impl_->changed.notify_all();
    return cancelled;
}

bool GpuFrameSlots::Drain()
{
    if (!impl_)
        return true;
    std::unique_lock<std::mutex> lock(impl_->mutex);
    if (impl_->retirement.RecordingCount() != 0)
        return false;
    impl_->changed.wait(lock,
                        [&]
                        {
                            return (impl_->backendLost || impl_->retirement.InFlightCount() == 0) &&
                                   impl_->callbacksRunning == 0;
                        });
    return !impl_->backendLost;
}

bool GpuFrameSlots::WaitInFlight()
{
    if (!impl_)
        return true;
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->changed.wait(lock,
                        [&]
                        {
                            return (impl_->backendLost || impl_->retirement.InFlightCount() == 0) &&
                                   impl_->callbacksRunning == 0;
                        });
    return !impl_->backendLost;
}

void GpuFrameSlots::Release()
{
    if (!impl_)
        return;
    Drain();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = true;
        impl_->changed.notify_all();
    }
    if (impl_->pump.joinable())
        impl_->pump.join();
    impl_->timing.Release();
    for (GpuFrameResources &frame : impl_->frames)
        ReleaseFrameResources(impl_->device, frame);
    impl_->released = true;
    delete impl_;
    impl_ = nullptr;
}

size_t GpuFrameSlots::InFlightCount() const
{
    if (!impl_)
        return 0;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->retirement.InFlightCount();
}

size_t GpuFrameSlots::Capacity() const
{
    return impl_ ? impl_->frames.size() : 0;
}

} // namespace gears::draw

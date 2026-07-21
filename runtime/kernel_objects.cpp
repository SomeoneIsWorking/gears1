#include "kernel_objects.h"

#include <chrono>

#include <lucent/log.h>

namespace gears
{

void KernelObject::Set()
{
    {
        std::lock_guard<std::mutex> guard(mutex_);
        signalled_ = true;
    }
    // A notification event releases every waiter; a synchronisation event
    // releases exactly one, which then consumes the signal.
    if (kind_ == Kind::NotificationEvent)
        cv_.notify_all();
    else
        cv_.notify_one();
}

void KernelObject::Clear()
{
    std::lock_guard<std::mutex> guard(mutex_);
    signalled_ = false;
}

void KernelObject::Pulse()
{
    // Releases whoever is waiting right now and leaves the object unsignalled;
    // a thread that arrives afterwards must wait.
    {
        std::lock_guard<std::mutex> guard(mutex_);
        signalled_ = true;
    }
    if (kind_ == Kind::NotificationEvent)
        cv_.notify_all();
    else
        cv_.notify_one();

    std::lock_guard<std::mutex> guard(mutex_);
    signalled_ = false;
}

bool KernelObject::Wait(int64_t timeout100ns)
{
    std::unique_lock<std::mutex> lock(mutex_);

    auto satisfied = [this] { return signalled_; };

    if (timeout100ns < 0)
    {
        cv_.wait(lock, satisfied);
    }
    else if (!cv_.wait_for(lock, std::chrono::nanoseconds(timeout100ns * 100), satisfied))
    {
        return false;
    }

    if (kind_ == Kind::SynchronizationEvent)
        signalled_ = false;
    return true;
}

uint32_t HandleTable::Insert(std::shared_ptr<KernelObject> object)
{
    std::lock_guard<std::mutex> guard(mutex_);
    const uint32_t handle = nextHandle_;
    nextHandle_ += 4;
    objects_[handle] = std::move(object);
    return handle;
}

std::shared_ptr<KernelObject> HandleTable::Lookup(uint32_t handle) const
{
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = objects_.find(handle);
    return it != objects_.end() ? it->second : nullptr;
}

bool HandleTable::Close(uint32_t handle)
{
    std::lock_guard<std::mutex> guard(mutex_);
    return objects_.erase(handle) != 0;
}

HandleTable& Handles()
{
    static HandleTable table;
    return table;
}

namespace
{
std::mutex g_resumeMutex;
std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> g_resumeGates;
} // namespace

void RegisterThreadResume(uint32_t handle, std::shared_ptr<KernelObject> resumed)
{
    if (!resumed)
        return;

    std::lock_guard<std::mutex> guard(g_resumeMutex);
    g_resumeGates[handle] = std::move(resumed);
}

void ResumeThread(uint32_t handle)
{
    std::shared_ptr<KernelObject> gate;
    {
        std::lock_guard<std::mutex> guard(g_resumeMutex);
        auto it = g_resumeGates.find(handle);
        if (it == g_resumeGates.end())
            return; // never suspended, so nothing to release
        gate = it->second;
        g_resumeGates.erase(it);
    }
    gate->Set();
}

} // namespace gears

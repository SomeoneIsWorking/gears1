#include "kernel_objects.h"

#include <chrono>

#include <lucent/log.h>

#include <byteswap.h>

#include "guest_heap.h"
#include "guest_memory.h"

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

int32_t KernelObject::Release(int32_t increment)
{
    int32_t previous;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        previous = count_;
        count_ += increment;
        if (limit_ > 0 && count_ > limit_)
            count_ = limit_;
        signalled_ = count_ > 0;
    }
    cv_.notify_all();
    return previous;
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

    auto satisfied = [this] { return kind_ == Kind::Semaphore ? count_ > 0 : signalled_; };

    if (timeout100ns < 0)
    {
        cv_.wait(lock, satisfied);
    }
    else if (!cv_.wait_for(lock, std::chrono::nanoseconds(timeout100ns * 100), satisfied))
    {
        return false;
    }

    if (kind_ == Kind::SynchronizationEvent)
    {
        signalled_ = false;
    }
    else if (kind_ == Kind::Semaphore)
    {
        --count_;
        signalled_ = count_ > 0;
    }
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
std::mutex g_guestObjectMutex;
std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> g_byGuestAddress;
std::unordered_map<uint32_t, uint32_t> g_handleToGuestAddress;

std::mutex g_resumeMutex;
std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> g_resumeGates;
} // namespace

uint32_t GuestAddressForHandle(uint32_t handle)
{
    auto object = Handles().Lookup(handle);
    if (!object)
        return 0;

    std::lock_guard<std::mutex> guard(g_guestObjectMutex);
    auto existing = g_handleToGuestAddress.find(handle);
    if (existing != g_handleToGuestAddress.end())
        return existing->second;

    // A dispatcher header the guest can hold a pointer to. Its contents are not
    // interpreted by the guest in any path reached so far; if that changes it
    // will show up as a wrong field read, not as silence.
    uint32_t size = 0x100;
    const uint32_t address = TitleHeap().Allocate(0, size, kMemCommit);
    if (address == 0)
        return 0;

    g_handleToGuestAddress[handle] = address;
    g_byGuestAddress[address] = std::move(object);
    return address;
}

std::shared_ptr<KernelObject> LookupByGuestAddress(uint32_t address)
{
    std::lock_guard<std::mutex> guard(g_guestObjectMutex);
    auto it = g_byGuestAddress.find(address);
    return it != g_byGuestAddress.end() ? it->second : nullptr;
}

namespace
{
// X_DISPATCH_HEADER: the first word packs the object type in its high byte,
// and the second word is the signal state.
constexpr uint32_t kDispatcherTypeNotificationEvent = 0;
constexpr uint32_t kDispatcherTypeSynchronizationEvent = 1;
constexpr uint32_t kDispatcherTypeSemaphore = 5;
} // namespace

void RegisterGuestObject(uint32_t address, std::shared_ptr<KernelObject> object)
{
    std::lock_guard<std::mutex> guard(g_guestObjectMutex);
    g_byGuestAddress[address] = std::move(object);
}

std::shared_ptr<KernelObject> BindGuestDispatcherObject(uint32_t address)
{
    {
        std::lock_guard<std::mutex> guard(g_guestObjectMutex);
        auto it = g_byGuestAddress.find(address);
        if (it != g_byGuestAddress.end())
            return it->second;
    }

    const uint32_t typeWord = ByteSwap(*Memory().Translate<uint32_t>(address));
    const int32_t signalState = int32_t(ByteSwap(*Memory().Translate<uint32_t>(address + 4)));
    const uint32_t type = typeWord >> 24;

    std::shared_ptr<KernelObject> object;
    switch (type)
    {
    case kDispatcherTypeNotificationEvent:
        object = std::make_shared<KernelObject>(
            KernelObject::Kind::NotificationEvent, signalState != 0);
        break;
    case kDispatcherTypeSynchronizationEvent:
        object = std::make_shared<KernelObject>(
            KernelObject::Kind::SynchronizationEvent, signalState != 0);
        break;
    case kDispatcherTypeSemaphore:
        object = std::make_shared<KernelObject>(signalState, 0);
        break;
    default:
        lucent::error("kernel", "dispatcher object at {:#x} has unhandled type {}", address, type);
        return nullptr;
    }

    lucent::debug("kernel", "bound guest dispatcher object at {:#x} (type {}, state {})",
        address, type, signalState);

    std::lock_guard<std::mutex> guard(g_guestObjectMutex);
    auto& slot = g_byGuestAddress[address];
    if (!slot)
        slot = object;
    return slot;
}

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

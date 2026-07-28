#include "kernel_objects.h"

#include <chrono>

#include <lucent/log.h>

#include <byteswap.h>

#include "guest_heap.h"
#include "guest_memory.h"

namespace gears
{

// ALL dispatcher-object state lives under this one lock, and every waiter waits
// on this one condition variable.
//
// It used to be a mutex and condvar per object, which is fine until something
// waits on more than one: KeWaitForMultipleObjects has to test every object and
// then consume the right ones ATOMICALLY, and per-object locks cannot give that
// without a lock-ordering scheme nobody would get right. One lock makes the
// multi-object wait obviously correct, and the contention is irrelevant at the
// rate a title signals events.
//
// The cost is that a signal wakes every waiter to re-check its own predicate,
// rather than only the ones that care. At a handful of objects that is cheaper
// than the bookkeeping needed to avoid it.
namespace
{
std::mutex g_dispatcherMutex;
std::condition_variable g_dispatcherCv;
} // namespace

const char* KernelObject::KindName() const
{
    switch (kind_)
    {
    case Kind::NotificationEvent: return "notification-event";
    case Kind::SynchronizationEvent: return "sync-event";
    case Kind::Semaphore: return "semaphore";
    case Kind::Mutant: return "mutant";
    }
    return "?";
}

bool KernelObject::SatisfiedLocked() const
{
    switch (kind_)
    {
    case Kind::Semaphore: return count_ > 0;
    case Kind::Mutant:
        return recursion_ == 0 || owner_ == std::this_thread::get_id();
    default: return signalled_;
    }
}

void KernelObject::ConsumeLocked()
{
    switch (kind_)
    {
    case Kind::SynchronizationEvent:
        signalled_ = false;
        break;
    case Kind::Semaphore:
        if (count_ > 0)
        {
            --count_;
            signalled_ = count_ > 0;
        }
        break;
    case Kind::Mutant:
        owner_ = std::this_thread::get_id();
        ++recursion_;
        break;
    default:
        break; // a notification event stays signalled until cleared
    }
}

int KernelObject::WaitMultiple(KernelObject* const* objects, size_t count,
                               bool waitAll, int64_t timeout100ns)
{
    if (count == 0)
        return -1;
    std::unique_lock<std::mutex> lock(g_dispatcherMutex);

    int satisfiedIndex = -1;
    auto ready = [&] {
        if (waitAll)
        {
            for (size_t i = 0; i < count; ++i)
                if (!objects[i] || !objects[i]->SatisfiedLocked())
                    return false;
            satisfiedIndex = 0;
            return true;
        }
        for (size_t i = 0; i < count; ++i)
            if (objects[i] && objects[i]->SatisfiedLocked())
            {
                satisfiedIndex = int(i);
                return true;
            }
        return false;
    };

    if (timeout100ns < 0)
    {
        g_dispatcherCv.wait(lock, ready);
    }
    else if (!g_dispatcherCv.wait_for(
                 lock, std::chrono::nanoseconds(timeout100ns * 100), ready))
    {
        return -1;
    }

    // Consume under the SAME lock acquisition that tested -- that atomicity is
    // the whole reason this function exists.
    if (waitAll)
    {
        for (size_t i = 0; i < count; ++i)
            if (objects[i])
                objects[i]->ConsumeLocked();
    }
    else if (satisfiedIndex >= 0)
    {
        objects[satisfiedIndex]->ConsumeLocked();
    }
    return satisfiedIndex;
}


void KernelObject::Set()
{
    {
        std::lock_guard<std::mutex> guard(g_dispatcherMutex);
        signalled_ = true;
    }
    // A notification event releases every waiter; a synchronisation event
    // releases exactly one, which then consumes the signal.
    if (kind_ == Kind::NotificationEvent)
        g_dispatcherCv.notify_all();
    else
        g_dispatcherCv.notify_all();
}

int32_t KernelObject::Release(int32_t increment)
{
    int32_t previous;
    {
        std::lock_guard<std::mutex> guard(g_dispatcherMutex);
        previous = count_;
        count_ += increment;
        if (limit_ > 0 && count_ > limit_)
            count_ = limit_;
        signalled_ = count_ > 0;
    }
    g_dispatcherCv.notify_all();
    return previous;
}

void KernelObject::Clear()
{
    std::lock_guard<std::mutex> guard(g_dispatcherMutex);
    signalled_ = false;
}

void KernelObject::Pulse()
{
    // Releases whoever is waiting right now and leaves the object unsignalled;
    // a thread that arrives afterwards must wait.
    {
        std::lock_guard<std::mutex> guard(g_dispatcherMutex);
        signalled_ = true;
    }
    if (kind_ == Kind::NotificationEvent)
        g_dispatcherCv.notify_all();
    else
        g_dispatcherCv.notify_all();

    std::lock_guard<std::mutex> guard(g_dispatcherMutex);
    signalled_ = false;
}

bool KernelObject::ReleaseMutant()
{
    {
        std::lock_guard<std::mutex> guard(g_dispatcherMutex);
        if (owner_ != std::this_thread::get_id())
            return false;
        if (--recursion_ > 0)
            return true;
        owner_ = {};
    }
    g_dispatcherCv.notify_all();
    return true;
}

bool KernelObject::Wait(int64_t timeout100ns)
{
    std::unique_lock<std::mutex> lock(g_dispatcherMutex);

    auto satisfied = [this] {
        switch (kind_)
        {
        case Kind::Semaphore: return count_ > 0;
        case Kind::Mutant:
            return recursion_ == 0 || owner_ == std::this_thread::get_id();
        default: return signalled_;
        }
    };

    if (timeout100ns < 0)
    {
        g_dispatcherCv.wait(lock, satisfied);
    }
    else if (!g_dispatcherCv.wait_for(lock, std::chrono::nanoseconds(timeout100ns * 100), satisfied))
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
    else if (kind_ == Kind::Mutant)
    {
        owner_ = std::this_thread::get_id();
        ++recursion_;
    }
    return true;
}

uint32_t HandleTable::Insert(std::shared_ptr<KernelObject> object)
{
    std::lock_guard<std::mutex> guard(g_dispatcherMutex);
    const uint32_t handle = nextHandle_;
    nextHandle_ += 4;
    objects_[handle] = std::move(object);
    return handle;
}

std::shared_ptr<KernelObject> HandleTable::Lookup(uint32_t handle) const
{
    std::lock_guard<std::mutex> guard(g_dispatcherMutex);
    auto it = objects_.find(handle);
    return it != objects_.end() ? it->second : nullptr;
}

bool HandleTable::Close(uint32_t handle)
{
    std::lock_guard<std::mutex> guard(g_dispatcherMutex);
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

void ResumeThread(uint32_t handleOrObject)
{
    // NtResumeThread is given a handle, KeResumeThread a pointer to the thread
    // object itself, and titles use both against the same thread. The guest
    // address is allocated lazily on the first ObReferenceObjectByHandle, so it
    // cannot be registered up front; instead an unrecognised key is treated as
    // an object address and mapped back to its handle.
    uint32_t key = handleOrObject;

    std::shared_ptr<KernelObject> gate;
    {
        std::lock_guard<std::mutex> guard(g_resumeMutex);
        auto it = g_resumeGates.find(key);
        if (it == g_resumeGates.end())
        {
            uint32_t handle = 0;
            {
                std::lock_guard<std::mutex> objectGuard(g_guestObjectMutex);
                for (const auto& [candidate, address] : g_handleToGuestAddress)
                {
                    if (address == handleOrObject)
                    {
                        handle = candidate;
                        break;
                    }
                }
            }

            if (handle == 0)
            {
                lucent::debug("thread", "resume of {:#x}: not a suspended thread",
                    handleOrObject);
                return; // never suspended, so nothing to release
            }

            key = handle;
            it = g_resumeGates.find(key);
            if (it == g_resumeGates.end())
                return;
        }

        gate = it->second;
        g_resumeGates.erase(it);
    }

    lucent::debug("thread", "resumed thread ({:#x})", handleOrObject);
    gate->Set();
}

} // namespace gears

#include "kernel_objects.h"

#include <chrono>

#include <lucent/log.h>

#include "byte_order.h"

#include "guest_heap.h"
#include "guest_memory.h"

namespace gears
{

// ALL dispatcher-object state lives under this one lock.
//
// It used to be a mutex per object, which is fine until something waits on more
// than one: KeWaitForMultipleObjects has to test every object and then consume
// the right ones ATOMICALLY, and per-object locks cannot give that without a
// lock-ordering scheme nobody would get right. One lock makes the multi-object
// wait obviously correct.
//
// The CONDITION VARIABLE is per waiter, registered on each object that waiter is
// blocked on -- see KernelObject::waiters_. One shared condition variable was
// simpler and made every signal wake every waiting guest thread; the title's
// audio is a two-thread ping-pong at 187.5 Hz, so that was 375 herd wakeups a
// second, and the handoff it paces measured 8.6 ms against a 5.3 ms budget.
// Waking only the waiters registered on the signalled object changes nothing
// about the atomicity: the state is still under the one lock.
namespace
{
std::mutex g_dispatcherMutex;

// Registers a waiter's condition variable on every object it is waiting for, and
// takes it off again on the way out -- including when the wait times out or the
// predicate throws. A waiter's condition variable lives on its own stack, so
// leaving one registered would be a dangling pointer in the next signal.
class RegisteredWaiter
{
  public:
    RegisteredWaiter(KernelObject *const *objects, size_t count, std::condition_variable &cv)
        : objects_(objects), count_(count), cv_(&cv)
    {
        for (size_t i = 0; i < count_; ++i)
            if (objects_[i])
                objects_[i]->AddWaiterLocked(cv_);
    }
    ~RegisteredWaiter()
    {
        for (size_t i = 0; i < count_; ++i)
            if (objects_[i])
                objects_[i]->RemoveWaiterLocked(cv_);
    }
    RegisteredWaiter(const RegisteredWaiter &) = delete;
    RegisteredWaiter &operator=(const RegisteredWaiter &) = delete;

  private:
    KernelObject *const *objects_;
    size_t count_;
    std::condition_variable *cv_;
};
} // namespace

void KernelObject::AddWaiterLocked(std::condition_variable *cv)
{
    waiters_.push_back(cv);
}

void KernelObject::RemoveWaiterLocked(std::condition_variable *cv)
{
    for (auto it = waiters_.begin(); it != waiters_.end(); ++it)
        if (*it == cv)
        {
            waiters_.erase(it);
            return;
        }
}

void KernelObject::WakeWaitersLocked()
{
    // notify_all on each: a waiter registers exactly one condition variable, so
    // this is one wakeup per thread actually waiting on THIS object.
    for (std::condition_variable *cv : waiters_)
        cv->notify_all();
}

const char *KernelObject::KindName() const
{
    switch (kind_)
    {
    case Kind::NotificationEvent:
        return "notification-event";
    case Kind::SynchronizationEvent:
        return "sync-event";
    case Kind::Semaphore:
        return "semaphore";
    case Kind::Mutant:
        return "mutant";
    }
    return "?";
}

bool KernelObject::SatisfiedLocked() const
{
    switch (kind_)
    {
    case Kind::Semaphore:
        return count_ > 0;
    case Kind::Mutant:
        return recursion_ == 0 || owner_ == std::this_thread::get_id();
    default:
        return signalled_;
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

int KernelObject::WaitMultiple(KernelObject *const *objects, size_t count, bool waitAll,
                               int64_t timeout100ns)
{
    if (count == 0)
        return -1;
    std::unique_lock<std::mutex> lock(g_dispatcherMutex);

    int satisfiedIndex = -1;
    auto ready = [&]
    {
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

    std::condition_variable cv;
    RegisteredWaiter registration(objects, count, cv);
    if (timeout100ns < 0)
    {
        cv.wait(lock, ready);
    }
    else if (!cv.wait_for(lock, std::chrono::nanoseconds(timeout100ns * 100), ready))
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
    // Notified with the lock HELD, and every waiter on this object is woken:
    // which of them gets through is decided by their predicates and by
    // ConsumeLocked, exactly as before. A notification event lets them all
    // through; a synchronisation event is consumed by whichever wakes first and
    // the rest go back to waiting.
    std::lock_guard<std::mutex> guard(g_dispatcherMutex);
    signalled_ = true;
    WakeWaitersLocked();
}

int32_t KernelObject::Release(int32_t increment)
{
    std::lock_guard<std::mutex> guard(g_dispatcherMutex);
    const int32_t previous = count_;
    count_ += increment;
    if (limit_ > 0 && count_ > limit_)
        count_ = limit_;
    signalled_ = count_ > 0;
    WakeWaitersLocked();
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
    // a thread that arrives afterwards must wait. The two lock scopes are kept
    // exactly as they were -- a waiter can only re-test its predicate once this
    // releases the lock, so a pulse can still be missed. That is a pre-existing
    // flaw in this emulation of KePulseEvent (the fix is a generation counter a
    // waiter compares against, not a notify), and it is left alone here so this
    // change is only about WHICH waiters are woken.
    {
        std::lock_guard<std::mutex> guard(g_dispatcherMutex);
        signalled_ = true;
        WakeWaitersLocked();
    }
    std::lock_guard<std::mutex> guard(g_dispatcherMutex);
    signalled_ = false;
}

bool KernelObject::ReleaseMutant()
{
    std::lock_guard<std::mutex> guard(g_dispatcherMutex);
    if (owner_ != std::this_thread::get_id())
        return false;
    if (--recursion_ > 0)
        return true;
    owner_ = {};
    WakeWaitersLocked();
    return true;
}

bool KernelObject::Wait(int64_t timeout100ns)
{
    std::unique_lock<std::mutex> lock(g_dispatcherMutex);

    auto satisfied = [this]
    {
        switch (kind_)
        {
        case Kind::Semaphore:
            return count_ > 0;
        case Kind::Mutant:
            return recursion_ == 0 || owner_ == std::this_thread::get_id();
        default:
            return signalled_;
        }
    };

    std::condition_variable cv;
    KernelObject *self = this;
    RegisteredWaiter registration(&self, 1, cv);
    if (timeout100ns < 0)
    {
        cv.wait(lock, satisfied);
    }
    else if (!cv.wait_for(lock, std::chrono::nanoseconds(timeout100ns * 100), satisfied))
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

HandleTable &Handles()
{
    static HandleTable table;
    return table;
}

namespace
{
std::mutex g_guestObjectMutex;
std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> g_byGuestAddress;
// The reverse of g_byGuestAddress, keyed by the OBJECT rather than by the handle
// it was first asked for through. It used to be keyed by handle, and that is
// wrong: NtDuplicateObject gives the title a second handle onto the SAME object,
// and a per-handle map then minted a second guest pointer for it. The title
// therefore held two different addresses for one object, and anything that
// recognised one of them did not recognise the other -- which is how one
// thread's KeSetAffinityThread went unrecognised in every run (catalog #42).
// The console has one object pointer per object, whatever handle you name it by.
std::unordered_map<const KernelObject *, uint32_t> g_addressByObject;

std::mutex g_resumeMutex;
std::unordered_map<const KernelObject *, std::shared_ptr<KernelObject>> g_resumeGates;
} // namespace

uint32_t GuestAddressForHandle(uint32_t handle)
{
    auto object = Handles().Lookup(handle);
    if (!object)
        return 0;

    std::lock_guard<std::mutex> guard(g_guestObjectMutex);
    auto existing = g_addressByObject.find(object.get());
    if (existing != g_addressByObject.end())
        return existing->second;

    // A dispatcher header the guest can hold a pointer to. Its contents are not
    // interpreted by the guest in any path reached so far; if that changes it
    // will show up as a wrong field read, not as silence.
    uint32_t size = 0x100;
    const uint32_t address = TitleHeap().Allocate(0, size, kMemCommit);
    if (address == 0)
        return 0;

    g_addressByObject[object.get()] = address;
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
        object =
            std::make_shared<KernelObject>(KernelObject::Kind::NotificationEvent, signalState != 0);
        break;
    case kDispatcherTypeSynchronizationEvent:
        object = std::make_shared<KernelObject>(KernelObject::Kind::SynchronizationEvent,
                                                signalState != 0);
        break;
    case kDispatcherTypeSemaphore:
        object = std::make_shared<KernelObject>(signalState, 0);
        break;
    default:
        lucent::error("kernel", "dispatcher object at {:#x} has unhandled type {}", address, type);
        return nullptr;
    }

    lucent::debug("kernel", "bound guest dispatcher object at {:#x} (type {}, state {})", address,
                  type, signalState);

    std::lock_guard<std::mutex> guard(g_guestObjectMutex);
    auto &slot = g_byGuestAddress[address];
    if (!slot)
        slot = object;
    return slot;
}

void RegisterThreadResume(const std::shared_ptr<KernelObject> &threadObject,
                          std::shared_ptr<KernelObject> resumed)
{
    if (!threadObject || !resumed)
        return;

    std::lock_guard<std::mutex> guard(g_resumeMutex);
    g_resumeGates[threadObject.get()] = std::move(resumed);
}

void ResumeThread(uint32_t handleOrObject)
{
    // NtResumeThread is given a handle, KeResumeThread a pointer to the thread
    // object itself, and titles use both against the same thread -- so both are
    // resolved to the OBJECT, which is the one identity both name and which
    // survives NtDuplicateObject. This used to key the gate by handle and, when
    // that missed, scan the address map backwards for a handle; a duplicated
    // handle defeated both halves of that.
    auto object = Handles().Lookup(handleOrObject);
    if (!object)
        object = LookupByGuestAddress(handleOrObject);
    if (!object)
    {
        lucent::debug("thread", "resume of {:#x}: names no kernel object", handleOrObject);
        return;
    }

    std::shared_ptr<KernelObject> gate;
    {
        std::lock_guard<std::mutex> guard(g_resumeMutex);
        auto it = g_resumeGates.find(object.get());
        if (it == g_resumeGates.end())
        {
            lucent::debug("thread", "resume of {:#x}: not a suspended thread", handleOrObject);
            return; // never suspended, so nothing to release
        }

        gate = it->second;
        g_resumeGates.erase(it);
    }

    lucent::debug("thread", "resumed thread ({:#x})", handleOrObject);
    gate->Set();
}

} // namespace gears

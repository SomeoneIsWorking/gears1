#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "kernel_status.h"

namespace gears
{

// A waitable kernel object. Events, semaphores and mutants all reduce to a
// signal count plus a rule for what a successful wait does to it, so they share
// one implementation rather than three near-identical ones.
class KernelObject
{
public:
    enum class Kind
    {
        NotificationEvent,   // stays signalled until explicitly cleared
        SynchronizationEvent // one waiter consumes the signal
    };

    KernelObject(Kind kind, bool signalled) : kind_(kind), signalled_(signalled) {}

    void Set();
    void Clear();
    void Pulse();

    // Returns true if the wait was satisfied, false on timeout.
    // A negative timeout means wait forever.
    bool Wait(int64_t timeout100ns);

private:
    Kind kind_;
    bool signalled_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

// Maps guest handles to host objects. Handles are opaque to the guest, so any
// stable non-zero value works; they are numbered from a high base so a handle
// mistaken for a pointer faults loudly instead of landing in real memory.
class HandleTable
{
public:
    uint32_t Insert(std::shared_ptr<KernelObject> object);
    std::shared_ptr<KernelObject> Lookup(uint32_t handle) const;
    bool Close(uint32_t handle);

private:
    mutable std::mutex mutex_;
    std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> objects_;
    uint32_t nextHandle_ = 0xF8000000;
};

HandleTable& Handles();

// Some kernel APIs take a pointer to the object itself rather than a handle
// (the console's dispatcher objects live in guest memory). Objects are given a
// guest address lazily, the first time one is actually asked for, so the common
// handle-only path costs nothing.
uint32_t GuestAddressForHandle(uint32_t handle);
std::shared_ptr<KernelObject> LookupByGuestAddress(uint32_t address);

// A suspended thread's resume gate, keyed by its handle. Kept beside the handle
// table because a thread handle waits on *exit*, so the resume signal needs its
// own home rather than overloading the same object.
void RegisterThreadResume(uint32_t handle, std::shared_ptr<KernelObject> resumed);
void ResumeThread(uint32_t handle);

} // namespace gears

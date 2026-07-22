#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
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
        NotificationEvent,    // stays signalled until explicitly cleared
        SynchronizationEvent, // one waiter consumes the signal
        Semaphore,            // signalled while count > 0; a waiter takes one
        Mutant                // owned by one thread, recursively acquirable
    };

    KernelObject(Kind kind, bool signalled) : kind_(kind), signalled_(signalled) {}
    KernelObject(int32_t count, int32_t limit)
        : kind_(Kind::Semaphore), signalled_(count > 0), count_(count), limit_(limit) {}

    void Set();
    void Clear();
    void Pulse();

    // Returns the previous count.
    int32_t Release(int32_t increment);

    // Mutant release. Returns false when the calling thread is not the owner,
    // which the caller reports as STATUS_MUTANT_NOT_OWNED.
    bool ReleaseMutant();

    // Returns true if the wait was satisfied, false on timeout.
    // A negative timeout means wait forever.
    bool Wait(int64_t timeout100ns);

private:
    Kind kind_;
    bool signalled_;
    int32_t count_ = 0;
    int32_t limit_ = 0;
    std::thread::id owner_{};       // Mutant only
    int32_t recursion_ = 0;         // Mutant only
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

// Binds a host object to a dispatcher object that lives in guest memory. Titles
// embed KEVENTs and KSEMAPHOREs in their own structures and initialise them in
// place, so these never pass through a handle. The object's kind and initial
// state are read from the guest's own dispatch header rather than assumed.
std::shared_ptr<KernelObject> BindGuestDispatcherObject(uint32_t address);
void RegisterGuestObject(uint32_t address, std::shared_ptr<KernelObject> object);

// A suspended thread's resume gate, keyed by its handle. Kept beside the handle
// table because a thread handle waits on *exit*, so the resume signal needs its
// own home rather than overloading the same object.
void RegisterThreadResume(uint32_t handle, std::shared_ptr<KernelObject> resumed);
void ResumeThread(uint32_t handle);

} // namespace gears

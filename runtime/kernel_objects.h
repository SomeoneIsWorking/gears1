#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

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

    // Wait on SEVERAL objects at once. `waitAll` false is WaitAny: return as
    // soon as one is satisfied, consuming only that one. `waitAll` true returns
    // only when every object is satisfied, and consumes them together.
    //
    // This cannot be built from Wait() in a loop. A synchronisation event or a
    // semaphore is CONSUMED by the waiter that takes it, so checking objects one
    // at a time can take a signal from an object the caller then abandons, and
    // WaitAll would consume some objects while blocking on another -- both lose
    // signals the guest is entitled to. The check and the consume have to be
    // atomic across every object, which is why all dispatcher state now lives
    // under one shared lock rather than a mutex per object.
    //
    // Returns the index of the object that satisfied a WaitAny, 0 for a
    // satisfied WaitAll, or -1 on timeout.
    static int WaitMultiple(KernelObject* const* objects, size_t count,
                            bool waitAll, int64_t timeout100ns);

    // For diagnostics. A wait that never returns is indistinguishable from a
    // wait that was never reached unless the log says WHAT was waited on, and
    // the kind is the part that says whether anything could have signalled it.
    Kind kind() const { return kind_; }
    const char* KindName() const;

    // Waiter registration. Public because the registration helper in the .cpp
    // drives it; every call requires the dispatcher lock to be held.
    void AddWaiterLocked(std::condition_variable* cv);
    void RemoveWaiterLocked(std::condition_variable* cv);

private:
    // Wake the waiters registered on this object. Caller holds the dispatcher
    // lock and must KEEP it across the call.
    void WakeWaitersLocked();

    // True when this object would let a waiter through. Caller holds the
    // dispatcher lock.
    bool SatisfiedLocked() const;
    // Take the signal, for the object kinds that consume one. Caller holds the
    // dispatcher lock.
    void ConsumeLocked();

    // The waiters currently blocked on THIS object, each waiting on its own
    // condition variable, all of them under the one dispatcher mutex.
    //
    // A single shared condition variable was correct and simple, and it cost the
    // audio path dearly: every signal woke every waiting guest thread to
    // re-evaluate its own predicate against the one dispatcher mutex. The title
    // mixes audio as a ping-pong between two threads at 187.5 Hz, so 375 signals
    // a second each woke a dozen threads that had nothing to do. Waking only the
    // waiters registered on the object keeps every atomicity argument above --
    // the state is still under one lock -- and stops the herd.
    //
    // Guarded by the dispatcher mutex; entries are stack objects owned by the
    // waiting threads, which is why notification happens with the lock HELD: a
    // waiter cannot leave and destroy its condition variable between the read
    // and the notify.
    std::vector<std::condition_variable*> waiters_;

    Kind kind_;
    bool signalled_;
    int32_t count_ = 0;
    int32_t limit_ = 0;
    std::thread::id owner_{};       // Mutant only
    int32_t recursion_ = 0;         // Mutant only
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
// The reverse: the object a guest address stands for, or null. The title passes
// these pointers back to APIs that name a thread (affinity among them), so the
// runtime has to be able to get back to what it created. It maps to the OBJECT,
// not to a handle, because the title duplicates handles and every duplicate
// names the same object.
std::shared_ptr<KernelObject> LookupByGuestAddress(uint32_t address);

// Binds a host object to a dispatcher object that lives in guest memory. Titles
// embed KEVENTs and KSEMAPHOREs in their own structures and initialise them in
// place, so these never pass through a handle. The object's kind and initial
// state are read from the guest's own dispatch header rather than assumed.
std::shared_ptr<KernelObject> BindGuestDispatcherObject(uint32_t address);
void RegisterGuestObject(uint32_t address, std::shared_ptr<KernelObject> object);

// A suspended thread's resume gate, keyed by the thread's dispatcher OBJECT --
// the same identity rule as above, so a duplicated handle still finds it. Kept
// beside the handle table because a thread object is what the guest waits on for
// *exit*, so the resume signal needs its own home rather than overloading it.
void RegisterThreadResume(const std::shared_ptr<KernelObject>& threadObject,
                          std::shared_ptr<KernelObject> resumed);
// Accepts either a handle or the guest address of the thread's object, because
// NtResumeThread is given one and KeResumeThread the other.
void ResumeThread(uint32_t handleOrObject);

} // namespace gears

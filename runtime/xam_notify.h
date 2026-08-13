#pragma once

#include <chrono>
#include <cstdint>

namespace gears
{

constexpr uint32_t kXNotificationSystemUI = 0x00000009;

// Broadcast a console notification to every existing listener whose area mask
// accepts it. Delayed notifications are materialised when the title polls, so
// the runtime never needs a detached host thread that can outlive shutdown.
void BroadcastNotification(uint32_t id, uint32_t param);
void ScheduleNotification(uint32_t id, uint32_t param,
    std::chrono::milliseconds delay);

// Test seam over the shipping queue. Production never resets global Xam state,
// but a process-local test must be able to drive both positive and negative
// classes independently.
void ResetNotificationsForTest();

} // namespace gears

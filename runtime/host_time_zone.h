// The host machine's time zone, expressed in the console's XCONFIG form.
//
// The seven XCONFIG_USER time-zone settings are exactly a Win32
// TIME_ZONE_INFORMATION taken apart: the title's XapiGetTimeZoneInformation
// (sub_827A98D0, scratch/ppc/ppc_recomp.119.cpp:2653) reads settings 0x01
// through 0x07 into a 172-byte structure whose field offsets are Bias(0),
// StandardName(4), StandardDate(68), StandardBias(84), DaylightName(88),
// DaylightDate(152), DaylightBias(168) -- i.e. TIME_ZONE_INFORMATION.
//
// EVERY ONE OF THE SEVEN IS FOUR BYTES. That is not an assumption: the guest
// passes a literal 4 in r6 (BufferSize) at all seven call sites, and Xenia's
// XConfigData::User (extern/xenia/src/xenia/kernel/xconfig.h:672) sizes the
// fields the same way -- `std::array<char, 4> tz_std_name` at 0x00C and
// `tz_dlt_name` at 0x010. The names are FOUR ASCII BYTES, not a string: the
// guest widens exactly four of them to UTF-16 and appends its own terminator,
// so a name shorter than four is zero-padded and a four-character name such as
// "CEST" carries no terminator at all.
//
// This is a PC port, so these come from the host's real time zone rather than
// a console default -- reporting UTC on a machine that knows it is in Berlin
// would put the title's clock an hour out for no reason.
#pragma once

#include <array>
#include <cstdint>

namespace gears
{

// One console time zone. Field meanings are Win32's, because that is what the
// title reassembles them into.
struct HostTimeZone
{
    // Minutes to ADD to local time to get UTC, so a zone west of Greenwich is
    // positive: the console's table stores 480 for Pacific (UTC-8)
    // (extern/xenia/src/xenia/kernel/xconfig.h, kTimezones).
    int32_t bias = 0;

    // Four ASCII bytes, zero-padded, NOT necessarily terminated.
    std::array<uint8_t, 4> stdName{};
    std::array<uint8_t, 4> dltName{};

    // A transition date, packed {month, week, dayOfWeek, hour}. The order is
    // the guest's: it stores byte 0 at SYSTEMTIME+2 (wMonth), byte 1 at +6
    // (wDay), byte 2 at +4 (wDayOfWeek) and byte 3 at +8 (wHour)
    // (ppc_recomp.119.cpp:2810-2830). `week` is the 1-based occurrence of that
    // weekday in the month, with 5 meaning "the last one" -- the same encoding
    // as Win32's SYSTEMTIME-in-a-TIME_ZONE_INFORMATION. All zero means the
    // zone has no daylight saving.
    std::array<uint8_t, 4> stdDate{};
    std::array<uint8_t, 4> dltDate{};

    // Extra minutes on top of `bias` while the respective time is in force.
    // Every entry in the console's own table has stdBias 0, because `bias` is
    // already the standard-time offset.
    int32_t stdBias = 0;
    int32_t dltBias = 0;

    bool observesDaylightSaving() const { return dltDate != std::array<uint8_t, 4>{}; }
};

// Derives the above from the host's current time zone, honouring TZ (it calls
// tzset). Uncached, so a test can set TZ and ask again; ConfigValue caches its
// own copy. Falls back to UTC with no daylight saving if the host's zone cannot
// be read at all, and says so through lucent rather than silently.
HostTimeZone QueryHostTimeZone();

} // namespace gears

#include "host_time_zone.h"

#include <cstring>
#include <ctime>
#include <string_view>

#include <lucent/log.h>

namespace gears
{
namespace
{

constexpr int kSecondsPerDay = 24 * 60 * 60;

// A probe of the host zone at one instant: the offset in force and the name
// that goes with it.
struct Probe
{
    bool valid = false;
    bool daylight = false;
    int32_t offsetSeconds = 0; // seconds EAST of UTC, tm_gmtoff's convention
    std::array<uint8_t, 4> name{};
};

// tm_gmtoff and tm_zone are the only portable-in-practice way to ask "what is
// this zone's offset and abbreviation at instant t"; they are POSIX.1-2024 and
// have been in glibc and the BSDs for decades. There is no ISO C equivalent --
// `timezone`/`tzname` are globals describing only the CURRENT instant, which
// cannot answer "was this January standard or daylight" for a southern zone.
Probe ProbeAt(std::time_t instant)
{
    Probe probe;
    std::tm local{};
    if (!localtime_r(&instant, &local))
        return probe;

    probe.valid = true;
    probe.daylight = local.tm_isdst > 0;
    probe.offsetSeconds = int32_t(local.tm_gmtoff);

    // Four ASCII bytes, zero-padded and NOT terminated -- the console's field
    // is a fixed four-byte array, so "CEST" fills it exactly. Abbreviations
    // longer than four characters do not occur in the zone database; a numeric
    // abbreviation such as "+0530" would, which is why this truncates rather
    // than assuming it fits.
    if (local.tm_zone)
    {
        const std::string_view name(local.tm_zone);
        std::memcpy(probe.name.data(), name.data(),
            name.size() < probe.name.size() ? name.size() : probe.name.size());
        if (name.size() > probe.name.size())
            lucent::warn("config", "host time zone abbreviation '{}' is longer"
                " than the console's four-byte field; truncated", name);
    }
    return probe;
}

// The instant at which the zone's daylight state changes, searched between two
// probes known to disagree. Bisection to the second: the transition is a step
// function of the instant, so this converges exactly.
std::time_t FindTransition(std::time_t before, std::time_t after)
{
    const bool beforeDaylight = ProbeAt(before).daylight;
    while (after - before > 1)
    {
        const std::time_t middle = before + (after - before) / 2;
        if (ProbeAt(middle).daylight == beforeDaylight)
            before = middle;
        else
            after = middle;
    }
    return after;
}

int DaysInMonth(int year, int month) // month is 1..12
{
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        return 29;
    return kDays[month - 1];
}

// Packs a transition instant into the console's {month, week, dayOfWeek, hour}.
//
// `offsetSeconds` is the offset in force BEFORE the transition, and that is the
// whole subtlety: Windows -- and therefore the console, and therefore the
// title -- states each rule in the wall clock that was running up to it. The EU
// switches at 01:00 UTC both ways, which is 02:00 CET going into summer and
// 03:00 CEST coming out; the console's table has exactly that pair
// (kEU_DltDate hour 2, kEU_StdDate hour 3, in
// extern/xenia/src/xenia/kernel/xconfig.h). Using the offset in force AFTER the
// transition would silently swap the two hours.
std::array<uint8_t, 4> PackDate(std::time_t transition, int32_t offsetSeconds)
{
    const std::time_t wall = transition + offsetSeconds;
    std::tm fields{};
    if (!gmtime_r(&wall, &fields))
        return {};

    const int month = fields.tm_mon + 1;
    // Week is the 1-based occurrence of this weekday in the month, with 5
    // meaning "the last one" -- so a rule that says "last Sunday" keeps saying
    // it in a month that has five of them.
    int week = (fields.tm_mday - 1) / 7 + 1;
    if (fields.tm_mday + 7 > DaysInMonth(fields.tm_year + 1900, month))
        week = 5;

    return {uint8_t(month), uint8_t(week), uint8_t(fields.tm_wday),
        uint8_t(fields.tm_hour)};
}

} // namespace

HostTimeZone QueryHostTimeZone()
{
    tzset(); // so a caller that has just set TZ gets the zone it asked for

    HostTimeZone zone;

    const std::time_t now = std::time(nullptr);
    std::tm nowFields{};
    if (now == std::time_t(-1) || !gmtime_r(&now, &nowFields))
    {
        lucent::error("config", "host clock unreadable; reporting UTC with no"
            " daylight saving to the title");
        return zone;
    }

    // Walk one whole year a day at a time. A year is the period of the rule,
    // and a day is fine enough to LOCATE a transition -- the exact instant then
    // comes from bisection. Probing only January and July would find the two
    // offsets but neither transition date, and the title needs those: it feeds
    // them to the dashboard's own local-time conversion.
    std::tm yearStart{};
    yearStart.tm_year = nowFields.tm_year;
    yearStart.tm_mday = 1;
    const std::time_t start = timegm(&yearStart);

    Probe standard;
    Probe daylight;
    std::time_t intoDaylight = 0;
    std::time_t intoStandard = 0;

    Probe previous = ProbeAt(start);
    if (!previous.valid)
    {
        lucent::error("config", "host time zone unreadable; reporting UTC with"
            " no daylight saving to the title");
        return zone;
    }
    if (previous.daylight)
        daylight = previous;
    else
        standard = previous;

    for (int day = 1; day <= 366; ++day)
    {
        const std::time_t instant = start + std::time_t(day) * kSecondsPerDay;
        const Probe probe = ProbeAt(instant);
        if (!probe.valid)
            break;

        if (probe.daylight != previous.daylight)
        {
            const std::time_t transition =
                FindTransition(instant - kSecondsPerDay, instant);
            if (probe.daylight && intoDaylight == 0)
                intoDaylight = transition;
            if (!probe.daylight && intoStandard == 0)
                intoStandard = transition;
        }

        if (probe.daylight && !daylight.valid)
            daylight = probe;
        if (!probe.daylight && !standard.valid)
            standard = probe;

        previous = probe;
    }

    if (!standard.valid)
    {
        // A zone the database says is on daylight time the whole year through.
        // There is no standard offset to report, so the observed one becomes
        // the bias and the title is told there is no daylight-saving rule --
        // which is true of the wall clock it will compute, even though the
        // abbreviation says otherwise.
        lucent::warn("config", "host time zone is on daylight time all year;"
            " reporting its offset as the standard bias with no DST rule");
        standard = daylight;
        daylight = Probe{};
    }

    zone.bias = -standard.offsetSeconds / 60; // console bias is minutes WEST
    zone.stdName = standard.name;
    zone.stdBias = 0; // `bias` already IS the standard offset, which is why
                      // every entry in the console's own table stores 0 here

    // Both transitions have to have been seen: a rule with only one end of it
    // is not a rule, and the title would apply the half it got.
    if (daylight.valid && intoDaylight != 0 && intoStandard != 0)
    {
        zone.dltName = daylight.name;
        zone.dltBias = -(daylight.offsetSeconds - standard.offsetSeconds) / 60;
        zone.dltDate = PackDate(intoDaylight, standard.offsetSeconds);
        zone.stdDate = PackDate(intoStandard, daylight.offsetSeconds);
    }

    return zone;
}

} // namespace gears

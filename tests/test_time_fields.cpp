// Tests for RtlTimeToTimeFields / RtlTimeFieldsToTime.
//
// These were verified only by "the title reached the campaign after they were
// implemented", which is evidence that they do not TRAP -- not that they compute
// the right date. A calendar conversion is exactly where an epoch constant or an
// off-by-one sits invisibly: every call still returns a plausible-looking date,
// and the consequence surfaces somewhere else entirely as a save with a wrong
// timestamp or a title that thinks a checkpoint is from the future.
//
// The anchors below are EXTERNAL facts, not values read out of this
// implementation, which is the only way a test like this can fail usefully:
//
//   * the Windows FILETIME epoch is 1601-01-01T00:00:00Z, so tick 0 is that
//     instant, and 1601-01-01 was a MONDAY (weekday 1 with 0 = Sunday);
//   * the Unix epoch 1970-01-01T00:00:00Z is 116444736000000000 ticks of 100 ns
//     after it, and 1970-01-01 was a THURSDAY (weekday 4);
//   * 2000-02-29 exists (a leap year: divisible by 400), while 1900-02-29 and
//     2100-02-29 do not -- the century rule is the classic place these break.

#include <cstdio>
#include <cstring>
#include <vector>

#include "ppc_config.h"
#include "ppc_context.h"

// The guest seam, so the byte-order and layout handling is exercised through the
// same code the title calls rather than through a reimplementation of it.
void __imp__RtlTimeToTimeFields(PPCContext& __restrict ctx, uint8_t* base);
void __imp__RtlTimeFieldsToTime(PPCContext& __restrict ctx, uint8_t* base);

PPCFuncMapping PPCFuncMappings[] = { { 0, nullptr } };

namespace
{

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok)
    {
        printf("FAIL %s\n", what);
        ++g_failures;
    }
}

constexpr int64_t kUnixEpochTicks = 116444736000000000;
constexpr int64_t kTicksPerSecond = 10000000;
constexpr int64_t kTicksPerDay = 86400 * kTicksPerSecond;

// A scratch guest address space. Two fixed addresses, deliberately non-zero so a
// missing base bias would be caught rather than passing by luck.
constexpr uint32_t kTimeAddress = 0x1000;
constexpr uint32_t kFieldsAddress = 0x1100;

uint16_t ReadBE16(const uint8_t* p)
{
    return uint16_t((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}

void StoreBE16(uint8_t* p, uint16_t v)
{
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}

void StoreBE64(uint8_t* p, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        p[i] = uint8_t(v >> (56 - i * 8));
}

uint64_t ReadBE64(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | p[i];
    return v;
}

struct Fields
{
    uint16_t year, month, day, hour, minute, second, ms, weekday;
};

Fields ToFields(std::vector<uint8_t>& memory, int64_t ticks)
{
    StoreBE64(memory.data() + kTimeAddress, uint64_t(ticks));
    std::memset(memory.data() + kFieldsAddress, 0xCD, 16);

    PPCContext ctx{};
    ctx.r3.u64 = kTimeAddress;
    ctx.r4.u64 = kFieldsAddress;
    __imp__RtlTimeToTimeFields(ctx, memory.data());

    const uint8_t* f = memory.data() + kFieldsAddress;
    return { ReadBE16(f + 0), ReadBE16(f + 2), ReadBE16(f + 4), ReadBE16(f + 6),
             ReadBE16(f + 8), ReadBE16(f + 10), ReadBE16(f + 12), ReadBE16(f + 14) };
}

int64_t FromFields(std::vector<uint8_t>& memory, const Fields& in, bool& ok)
{
    uint8_t* f = memory.data() + kFieldsAddress;
    StoreBE16(f + 0, in.year);   StoreBE16(f + 2, in.month);
    StoreBE16(f + 4, in.day);    StoreBE16(f + 6, in.hour);
    StoreBE16(f + 8, in.minute); StoreBE16(f + 10, in.second);
    StoreBE16(f + 12, in.ms);    StoreBE16(f + 14, in.weekday);
    StoreBE64(memory.data() + kTimeAddress, 0);

    PPCContext ctx{};
    ctx.r3.u64 = kFieldsAddress;
    ctx.r4.u64 = kTimeAddress;
    __imp__RtlTimeFieldsToTime(ctx, memory.data());
    ok = ctx.r3.u32 != 0;
    return int64_t(ReadBE64(memory.data() + kTimeAddress));
}

// Tick 0 IS the epoch, and 1601-01-01 was a Monday. Both facts are external to
// this code.
void TestEpochItself()
{
    std::vector<uint8_t> memory(0x4000, 0);
    const Fields f = ToFields(memory, 0);
    Check(f.year == 1601 && f.month == 1 && f.day == 1,
        "epoch: tick 0 is 1601-01-01");
    Check(f.hour == 0 && f.minute == 0 && f.second == 0 && f.ms == 0,
        "epoch: tick 0 is midnight");
    Check(f.weekday == 1, "epoch: 1601-01-01 was a Monday (1, with 0 = Sunday)");
}

// The Unix epoch is a second independent anchor, and it pins the delta constant.
void TestUnixEpoch()
{
    std::vector<uint8_t> memory(0x4000, 0);
    const Fields f = ToFields(memory, kUnixEpochTicks);
    Check(f.year == 1970 && f.month == 1 && f.day == 1,
        "unix: 116444736000000000 ticks is 1970-01-01 -- the epoch delta is right");
    Check(f.hour == 0 && f.minute == 0 && f.second == 0,
        "unix: and it is midnight, so no timezone has been applied");
    Check(f.weekday == 4, "unix: 1970-01-01 was a Thursday (4)");
}

// The century rule. 2000 is a leap year, 1900 and 2100 are not, and a conversion
// that gets this wrong is off by a day for months afterwards.
void TestLeapYearCenturyRule()
{
    std::vector<uint8_t> memory(0x4000, 0);

    // 2000-02-29T00:00:00Z, built by counting from the Unix epoch: 30 years of
    // which 1972..1996 contribute 7 leap days, plus Jan (31) and 28 days of Feb.
    const int64_t days1970to2000 = 30 * 365 + 7;   // 1972,76,80,84,88,92,96 -> 7
    const int64_t toFeb29 = days1970to2000 + 31 + 28;
    const Fields f = ToFields(memory, kUnixEpochTicks + toFeb29 * kTicksPerDay);
    Check(f.year == 2000 && f.month == 2 && f.day == 29,
        "leap: 2000-02-29 exists and lands on the right date");

    // And the inverse must REJECT a day that does not exist.
    bool ok = false;
    FromFields(memory, Fields{1900, 2, 29, 0, 0, 0, 0, 0}, ok);
    Check(!ok, "leap: 1900-02-29 is rejected -- 1900 is not a leap year");
    FromFields(memory, Fields{2100, 2, 29, 0, 0, 0, 0, 0}, ok);
    Check(!ok, "leap: 2100-02-29 is rejected too");
    FromFields(memory, Fields{2000, 2, 29, 0, 0, 0, 0, 0}, ok);
    Check(ok, "leap: 2000-02-29 is ACCEPTED -- the rule is not just 'reject Feb 29'");
}

// Sub-second and end-of-day handling, where a floor/round mistake shows up.
void TestTimeOfDayEdges()
{
    std::vector<uint8_t> memory(0x4000, 0);

    const Fields lastTick = ToFields(memory, kUnixEpochTicks + kTicksPerDay - 1);
    Check(lastTick.year == 1970 && lastTick.month == 1 && lastTick.day == 1,
        "edge: one tick before midnight is still the FIRST day, not the second");
    Check(lastTick.hour == 23 && lastTick.minute == 59 && lastTick.second == 59,
        "edge: and it is 23:59:59");
    Check(lastTick.ms == 999, "edge: with 999 ms -- the sub-second part floors");

    const Fields midnight = ToFields(memory, kUnixEpochTicks + kTicksPerDay);
    Check(midnight.day == 2 && midnight.hour == 0 && midnight.ms == 0,
        "edge: exactly one day later is the second day at midnight");
}

// A round trip must be exact at millisecond resolution, which is all the fields
// carry. This is the property the title actually depends on: it converts one way
// and back.
void TestRoundTrip()
{
    std::vector<uint8_t> memory(0x4000, 0);
    const int64_t samples[] = {
        0,
        kUnixEpochTicks,
        kUnixEpochTicks + 1234567 * kTicksPerSecond + 4560000,
        kUnixEpochTicks - 100 * kTicksPerDay,   // pre-1970, the negative path
        kUnixEpochTicks + 20000 * kTicksPerDay,
    };
    for (const int64_t ticks : samples)
    {
        const Fields f = ToFields(memory, ticks);
        bool ok = false;
        const int64_t back = FromFields(memory, f, ok);
        Check(ok, "round trip: the inverse accepts fields the forward pass produced");
        // The fields hold milliseconds, so the round trip is exact only to that.
        const int64_t expected = (ticks / 10000) * 10000;
        Check(back == expected,
            "round trip: ticks -> fields -> ticks is exact at ms resolution");
    }
}

// Pre-1970 must not be mishandled: the implementation floors deliberately so a
// negative tick count lands on the right calendar day rather than truncating
// toward zero.
void TestPreUnixEpoch()
{
    std::vector<uint8_t> memory(0x4000, 0);
    // 1969-12-31T23:59:59.999Z is one millisecond before the Unix epoch.
    const Fields f = ToFields(memory, kUnixEpochTicks - 10000);
    Check(f.year == 1969 && f.month == 12 && f.day == 31,
        "pre-epoch: one ms before 1970 is 1969-12-31, not 1970-01-01");
    Check(f.hour == 23 && f.minute == 59 && f.second == 59 && f.ms == 999,
        "pre-epoch: and the time of day floors correctly for a negative value");
}

} // namespace

int main()
{
    TestEpochItself();
    TestUnixEpoch();
    TestLeapYearCenturyRule();
    TestTimeOfDayEdges();
    TestRoundTrip();
    TestPreUnixEpoch();

    if (g_failures == 0)
    {
        printf("all time field tests passed\n");
        return 0;
    }
    printf("%d time field test(s) FAILED\n", g_failures);
    return 1;
}

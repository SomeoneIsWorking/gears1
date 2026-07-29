// Tests for the console configuration table.
//
// These are bare numbers with no structure to check them against, so a shifted
// table is invisible: every lookup still "works", it just answers a different
// question than the one asked. That is what had happened -- language sat at
// 0x07 and video flags at 0x08, so the title asking for LANGUAGE (0x09) was
// answered with the audio-flags entry, and got 0 where English is 1.
//
// The identifiers below are the console's, transcribed in Xenia's
// kernel/xconfig.h. The test exists to pin the NAMES to the NUMBERS, because
// that mapping is the whole content of this table and nothing else in the
// runtime can catch it being wrong.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ppc_config.h"
#include "ppc_context.h"

#include "host_time_zone.h"
#include "xconfig.h"

// The guest seam, so the buffer-size negotiation is tested through the code the
// title actually calls rather than through a copy of it.
void __imp__ExGetXConfigSetting(PPCContext& __restrict ctx, uint8_t* base);

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

uint16_t ReadBE16(const uint8_t* p)
{
    return uint16_t((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}

uint32_t ReadBE32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Fetches a setting as a 32-bit value, failing the test if it is absent.
bool Value(uint16_t category, uint16_t setting, uint32_t& out)
{
    const auto found = gears::ConfigValue(category, setting);
    if (!found || found->bytes.size() != 4)
        return false;
    out = ReadBE32(found->bytes.data());
    return true;
}

// THE TWO THE TITLE ACTUALLY ASKS FOR, measured on a real run. Getting these
// two wrong is not theoretical -- it happened.
void TestSettingsTheTitleQueries()
{
    uint32_t language = 0;
    Check(Value(gears::kConfigUserCategory, gears::kConfigUserLanguage, language),
        "language: setting 0x09 is answered");
    Check(language == gears::kLanguageEnglish,
        "language: 0x09 is LANGUAGE and reads English (1), not 0");

    uint32_t videoFlags = 0;
    Check(Value(gears::kConfigUserCategory, gears::kConfigUserVideoFlags, videoFlags),
        "video: setting 0x0A is answered");
}

// The numbering itself. Each of these was wrong before by exactly the amount
// that made a neighbouring setting answer in its place.
void TestSettingIdentifiers()
{
    Check(gears::kConfigUserTimeZoneBias == 0x01, "id: time zone bias is 0x01");
    Check(gears::kConfigUserTimeZoneStdName == 0x02, "id: tz std name is 0x02");
    Check(gears::kConfigUserLanguage == 0x09, "id: language is 0x09, not 0x07");
    Check(gears::kConfigUserVideoFlags == 0x0A, "id: video flags is 0x0A, not 0x08");
    Check(gears::kConfigUserAudioFlags == 0x0B, "id: audio flags is 0x0B, not 0x09");
    Check(gears::kConfigUserRetailFlags == 0x0C, "id: retail flags is 0x0C, not 0x0A");
    Check(gears::kConfigUserCountry == 0x0E, "id: country is 0x0E");
}

// A setting nothing has implemented must be reported absent rather than
// answered with a neighbour's value -- which is precisely the failure mode this
// table had.
void TestUnknownSettingsAreAbsent()
{
    Check(!gears::ConfigValue(gears::kConfigUserCategory, 0x00),
        "unknown: setting 0 is absent");
    Check(!gears::ConfigValue(gears::kConfigUserCategory, 0xFF),
        "unknown: an out-of-range setting is absent");
    Check(!gears::ConfigValue(0x99, gears::kConfigUserLanguage),
        "unknown: a valid setting in the wrong category is absent");
}

// Every answered setting must report a size, because the caller uses it to size
// its buffer and a zero-length answer is not a value.
void TestEverySettingHasABody()
{
    for (uint16_t setting = 0; setting <= 0x20; ++setting)
    {
        const auto found = gears::ConfigValue(gears::kConfigUserCategory, setting);
        if (found)
            Check(!found->bytes.empty(),
                "body: an answered setting is never zero-length");
    }
}

// --------------------------------------------------------------------------
// Time zone.
//
// The seven XCONFIG_USER time-zone settings are a Win32 TIME_ZONE_INFORMATION
// taken apart, and the title reassembles it in XapiGetTimeZoneInformation
// (sub_827A98D0, scratch/ppc/ppc_recomp.119.cpp:2653). That function is the
// contract these tests encode: it passes a literal BufferSize of 4 at all
// seven call sites, and it aborts the whole gather the moment one of them
// returns a negative status -- which is why refusing setting 0x02 cost the
// title its entire time zone.
// --------------------------------------------------------------------------

// Re-reads the host zone with TZ pinned, so the expectations below are about
// the derivation and not about whatever zone this machine happens to sit in.
gears::HostTimeZone ZoneOf(const char* tz)
{
    setenv("TZ", tz, 1);
    return gears::QueryHostTimeZone();
}

bool NameIs(const std::array<uint8_t, 4>& field, const char* expected)
{
    std::array<uint8_t, 4> want{};
    std::memcpy(want.data(), expected, std::strlen(expected));
    return field == want;
}

bool DateIs(const std::array<uint8_t, 4>& field, uint8_t month, uint8_t week,
    uint8_t dayOfWeek, uint8_t hour)
{
    return field == std::array<uint8_t, 4>{month, week, dayOfWeek, hour};
}

// EVERY time-zone setting is four bytes wide -- the guest's own BufferSize
// argument says so, and Xenia's XConfigData::User sizes tz_std_name and
// tz_dlt_name as `std::array<char, 4>`. A name is therefore four ASCII bytes,
// not a string: a shorter one is zero-padded, and a four-character one such as
// "CEST" is not terminated at all. The guest copies exactly four bytes and
// appends its own wide terminator (ppc_recomp.119.cpp:2732-2748).
void TestTimeZoneSettingWidths()
{
    static const uint16_t kTimeZoneSettings[] = {
        gears::kConfigUserTimeZoneBias,    gears::kConfigUserTimeZoneStdName,
        gears::kConfigUserTimeZoneDltName, gears::kConfigUserTimeZoneStdDate,
        gears::kConfigUserTimeZoneDltDate, gears::kConfigUserTimeZoneStdBias,
        gears::kConfigUserTimeZoneDltBias,
    };

    for (const uint16_t setting : kTimeZoneSettings)
    {
        const auto found =
            gears::ConfigValue(gears::kConfigUserCategory, setting);
        if (!found)
        {
            printf("FAIL tz: setting %#x is refused, which aborts the guest's"
                   " whole time-zone gather\n", setting);
            ++g_failures;
            continue;
        }
        Check(found->bytes.size() == 4,
            "tz: every time-zone setting is exactly 4 bytes");
    }
}

void TestTimeZoneIdentifiers()
{
    Check(gears::kConfigUserTimeZoneStdName == 0x02, "id: tz std name is 0x02");
    Check(gears::kConfigUserTimeZoneDltName == 0x03, "id: tz dlt name is 0x03");
    Check(gears::kConfigUserTimeZoneStdDate == 0x04, "id: tz std date is 0x04");
    Check(gears::kConfigUserTimeZoneDltDate == 0x05, "id: tz dlt date is 0x05");
    Check(gears::kConfigUserTimeZoneStdBias == 0x06, "id: tz std bias is 0x06");
    Check(gears::kConfigUserTimeZoneDltBias == 0x07, "id: tz dlt bias is 0x07");
}

// The console's own table is the oracle here. Xenia transcribes the dashboard's
// 75 time zones (extern/xenia/src/xenia/kernel/xconfig.h, kTimezones), so a
// derivation from the host zone can be checked against the entry the console
// would have used for the same place -- if the host-derived bytes for Berlin do
// not equal the console's "GMT+01 Amsterdam/Berlin" entry, the derivation is
// wrong, not merely different.
void TestHostZoneMatchesTheConsoleTable()
{
    // Pacific: bias 480 (positive is WEST), "PST"/"PDT", and since 2007 the
    // US rule is second Sunday of March to first Sunday of November, 02:00.
    const auto pacific = ZoneOf("America/Los_Angeles");
    Check(pacific.bias == 480, "tz LA: bias is +480 minutes (UTC-8)");
    Check(NameIs(pacific.stdName, "PST"), "tz LA: std name is PST, zero-padded");
    Check(NameIs(pacific.dltName, "PDT"), "tz LA: dlt name is PDT, zero-padded");
    Check(pacific.stdBias == 0, "tz LA: std bias is 0, the console's convention");
    Check(pacific.dltBias == -60, "tz LA: dlt bias is -60, as the console table has it");
    Check(DateIs(pacific.dltDate, 3, 2, 0, 2),
        "tz LA: DST starts 2nd Sunday of March at 02:00");
    Check(DateIs(pacific.stdDate, 11, 1, 0, 2),
        "tz LA: DST ends 1st Sunday of November at 02:00");

    // Berlin is the direct cross-check: the console's kEU_StdDate is
    // {0x0A,0x05,0x00,0x03} and kEU_DltDate is {0x03,0x05,0x00,0x02}. The two
    // differ in the HOUR because a transition is expressed in the wall clock in
    // force BEFORE it -- 02:00 CET going in, 03:00 CEST coming out. A
    // derivation that used the wrong side would give 03:00/02:00 and pass every
    // other check here.
    const auto berlin = ZoneOf("Europe/Berlin");
    Check(berlin.bias == -60, "tz Berlin: bias is -60 minutes (UTC+1)");
    Check(NameIs(berlin.stdName, "CET"), "tz Berlin: std name is CET");
    Check(NameIs(berlin.dltName, "CEST"),
        "tz Berlin: dlt name is CEST -- four characters, so NOT terminated");
    Check(berlin.dltBias == -60, "tz Berlin: dlt bias is -60");
    Check(DateIs(berlin.dltDate, 3, 5, 0, 2),
        "tz Berlin: kEU_DltDate -- last Sunday of March at 02:00 CET");
    Check(DateIs(berlin.stdDate, 10, 5, 0, 3),
        "tz Berlin: kEU_StdDate -- last Sunday of October at 03:00 CEST");

    // A zone with no daylight saving at all, and a half-hour offset: the
    // console stores zeros for the daylight name, both dates and the daylight
    // bias (TimeZone::NoDST in Xenia's table).
    const auto kolkata = ZoneOf("Asia/Kolkata");
    Check(kolkata.bias == -330, "tz Kolkata: bias is -330 minutes (UTC+5:30)");
    Check(!kolkata.observesDaylightSaving(), "tz Kolkata: no daylight saving");
    Check(NameIs(kolkata.dltName, ""), "tz Kolkata: dlt name is all zeros");
    Check(DateIs(kolkata.stdDate, 0, 0, 0, 0), "tz Kolkata: std date is all zeros");
    Check(DateIs(kolkata.dltDate, 0, 0, 0, 0), "tz Kolkata: dlt date is all zeros");
    Check(kolkata.dltBias == 0, "tz Kolkata: dlt bias is 0");

    const auto utc = ZoneOf("UTC");
    Check(utc.bias == 0, "tz UTC: bias is 0");
    Check(!utc.observesDaylightSaving(), "tz UTC: no daylight saving");

    // Southern hemisphere: standard time falls in the middle of the year, so a
    // derivation that assumed January is always standard would report Sydney as
    // permanently on daylight time and get the bias an hour out.
    const auto sydney = ZoneOf("Australia/Sydney");
    Check(sydney.bias == -600, "tz Sydney: bias is -600 minutes (UTC+10 standard)");
    Check(sydney.observesDaylightSaving(), "tz Sydney: observes daylight saving");
    Check(sydney.dltBias == -60, "tz Sydney: dlt bias is -60");
}

// A name is four raw bytes with no terminator required, but everything past the
// name must be zero -- the guest widens all four bytes into its UTF-16 field,
// so a stale byte becomes a garbage character in the zone name it displays.
void TestNamesAreZeroPaddedNotTruncatedText()
{
    for (const char* zone : {"America/Los_Angeles", "Europe/Berlin",
             "Asia/Kolkata", "UTC", "Australia/Sydney"})
    {
        const auto tz = ZoneOf(zone);
        bool seenPad = false;
        bool ok = true;
        for (const uint8_t byte : tz.stdName)
        {
            if (byte == 0)
                seenPad = true;
            else if (seenPad)
                ok = false; // a non-zero byte after a pad byte
        }
        Check(ok, "tz: a std name is ASCII then zeros, never zeros then ASCII");
    }
}

// The size negotiation at the guest seam. The title asks with a 4-byte buffer,
// which is exactly the width, so the success path is the one it takes -- but
// the required size must still come back for a caller that asks with less.
void TestGuestSeamSizeProtocol()
{
    std::vector<uint8_t> memory(0x100, 0xCD);
    PPCContext ctx{};

    const uint32_t bufferAddress = 0x40;
    const uint32_t requiredSizeAddress = 0x20;

    auto call = [&](uint16_t setting, uint32_t bufferSize) {
        ctx.r3.u64 = gears::kConfigUserCategory;
        ctx.r4.u64 = setting;
        ctx.r5.u64 = bufferAddress;
        ctx.r6.u64 = bufferSize;
        ctx.r7.u64 = requiredSizeAddress;
        __imp__ExGetXConfigSetting(ctx, memory.data());
        return uint32_t(ctx.r3.u64);
    };

    // The width the title actually passes.
    Check(call(gears::kConfigUserTimeZoneStdName, 4) == 0,
        "seam: the title's own 4-byte buffer is accepted for the std name");
    Check(ReadBE16(&memory[requiredSizeAddress]) == 4,
        "seam: the required size is reported as 4");

    // A short buffer is the one case that must fail, and it must fail with
    // STATUS_BUFFER_TOO_SMALL plus the size to retry with -- an error alone
    // tells the caller nothing about how much to allocate.
    Check(call(gears::kConfigUserTimeZoneStdName, 2) == 0xC0000023,
        "seam: a 2-byte buffer gets STATUS_BUFFER_TOO_SMALL");
    Check(ReadBE16(&memory[requiredSizeAddress]) == 4,
        "seam: the required size is still reported when the buffer is too small");

    // The guest walks settings 0x01..0x07 and abandons the gather on the first
    // negative status, so all seven have to answer.
    for (uint16_t setting = gears::kConfigUserTimeZoneBias;
         setting <= gears::kConfigUserTimeZoneDltBias; ++setting)
        Check(call(setting, 4) == 0,
            "seam: every setting the guest's gather walks is answered");
}

} // namespace

int main()
{
    TestSettingIdentifiers();
    TestSettingsTheTitleQueries();
    TestUnknownSettingsAreAbsent();
    TestEverySettingHasABody();
    TestTimeZoneIdentifiers();
    TestTimeZoneSettingWidths();
    TestHostZoneMatchesTheConsoleTable();
    TestNamesAreZeroPaddedNotTruncatedText();
    TestGuestSeamSizeProtocol();

    if (g_failures == 0)
    {
        printf("all xconfig tests passed\n");
        return 0;
    }
    printf("%d xconfig test(s) FAILED\n", g_failures);
    return 1;
}

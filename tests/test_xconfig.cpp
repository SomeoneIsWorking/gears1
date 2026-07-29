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

#include <cstdio>

#include "ppc_config.h"
#include "ppc_context.h"

#include "xconfig.h"

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

} // namespace

int main()
{
    TestSettingIdentifiers();
    TestSettingsTheTitleQueries();
    TestUnknownSettingsAreAbsent();
    TestEverySettingHasABody();

    if (g_failures == 0)
    {
        printf("all xconfig tests passed\n");
        return 0;
    }
    printf("%d xconfig test(s) FAILED\n", g_failures);
    return 1;
}

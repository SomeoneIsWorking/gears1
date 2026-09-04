#include "xconfig.h"

#include "host_time_zone.h"

namespace gears
{
namespace
{

// Big-endian, because the guest reads these as its own words.
ConfigSetting Dword(uint32_t value)
{
    return {{uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)}};
}

// A time-zone name or transition date: four raw bytes with no byte order of
// their own -- original guest routine 0x827A98D0 copies them one at a time
// rather than loading a word, so byte-swapping them would corrupt them.
ConfigSetting Raw(const std::array<uint8_t, 4> &bytes)
{
    return {{bytes.begin(), bytes.end()}};
}

// Derived once. The host's zone can change under a running process in
// principle, but the title reads these settings once while loading and builds
// its own TIME_ZONE_INFORMATION from them, so re-deriving per call would only
// let the seven answers disagree with each other across a transition.
const HostTimeZone &CachedHostTimeZone()
{
    static const HostTimeZone zone = QueryHostTimeZone();
    return zone;
}

} // namespace

std::optional<ConfigSetting> ConfigValue(uint16_t category, uint16_t setting)
{
    if (category == kConfigUserCategory)
    {
        switch (setting)
        {
        // The seven time-zone settings are one answer, not seven: the title's
        // XapiGetTimeZoneInformation (sub_827A98D0) reads 0x01 through 0x07 in
        // order into a Win32 TIME_ZONE_INFORMATION and ABANDONS THE WHOLE
        // GATHER on the first negative status. Refusing any one of them --
        // which is what happened to 0x02 -- costs the title its time zone
        // entirely, not just that field.
        //
        // The bias used to be a flat 0 here, i.e. "the console is in
        // Greenwich". This is a PC port and the host knows better.
        case kConfigUserTimeZoneBias:
            return Dword(uint32_t(CachedHostTimeZone().bias));
        case kConfigUserTimeZoneStdName:
            return Raw(CachedHostTimeZone().stdName);
        case kConfigUserTimeZoneDltName:
            return Raw(CachedHostTimeZone().dltName);
        case kConfigUserTimeZoneStdDate:
            return Raw(CachedHostTimeZone().stdDate);
        case kConfigUserTimeZoneDltDate:
            return Raw(CachedHostTimeZone().dltDate);
        case kConfigUserTimeZoneStdBias:
            return Dword(uint32_t(CachedHostTimeZone().stdBias));
        case kConfigUserTimeZoneDltBias:
            return Dword(uint32_t(CachedHostTimeZone().dltBias));
        case kConfigUserLanguage:
            return Dword(kLanguageEnglish);
        case kConfigUserVideoFlags:
            return Dword(0x00040000);
        case kConfigUserAudioFlags:
            return Dword(0); // stereo
        case kConfigUserRetailFlags:
            return Dword(1);
        case kConfigUserCountry:
            return Dword(kCountryUnitedStates);
        default:
            break;
        }
    }
    else if (category == kConfigSecuredCategory)
    {
        switch (setting)
        {
        case kConfigSecuredAvRegion:
            return Dword(0x00001000);
        default:
            break;
        }
    }

    return std::nullopt;
}

} // namespace gears

#include "xconfig.h"

namespace gears
{
namespace
{

// Big-endian, because the guest reads these as its own words.
ConfigSetting Dword(uint32_t value)
{
    return {{uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8),
             uint8_t(value)}};
}

} // namespace

std::optional<ConfigSetting> ConfigValue(uint16_t category, uint16_t setting)
{
    if (category == kConfigUserCategory)
    {
        switch (setting)
        {
        case kConfigUserTimeZoneBias:
            return Dword(0);
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

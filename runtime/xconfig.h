// Console configuration settings.
//
// These identifiers are the console's, transcribed in Xenia's
// kernel/xconfig.h. They are bare numbers with no structure to check them
// against, so a shifted table is invisible in operation: every lookup still
// succeeds, it just answers a different question than the one asked. That had
// already happened here -- language sat at 0x07, so the title asking for
// LANGUAGE was answered with the audio-flags entry and read 0 where English
// is 1.
//
// Hence the named constants and the test that pins them. The mapping from name
// to number IS the content of this table; nothing else in the runtime can
// catch it being wrong.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace gears
{

constexpr uint16_t kConfigSecuredCategory = 0x0002;
constexpr uint16_t kConfigUserCategory = 0x0003;

constexpr uint16_t kConfigUserTimeZoneBias = 0x01;
constexpr uint16_t kConfigUserTimeZoneStdName = 0x02;
constexpr uint16_t kConfigUserTimeZoneDltName = 0x03;
constexpr uint16_t kConfigUserTimeZoneStdBias = 0x06;
constexpr uint16_t kConfigUserTimeZoneDltBias = 0x07;
constexpr uint16_t kConfigUserDefaultProfile = 0x08;
constexpr uint16_t kConfigUserLanguage = 0x09;
constexpr uint16_t kConfigUserVideoFlags = 0x0A;
constexpr uint16_t kConfigUserAudioFlags = 0x0B;
constexpr uint16_t kConfigUserRetailFlags = 0x0C;
constexpr uint16_t kConfigUserDevkitFlags = 0x0D;
constexpr uint16_t kConfigUserCountry = 0x0E;

constexpr uint16_t kConfigSecuredAvRegion = 0x0002;

constexpr uint32_t kLanguageEnglish = 1;
constexpr uint32_t kCountryUnitedStates = 103;

// NOT EVERY SETTING IS FOUR BYTES -- the time-zone names are strings -- so a
// setting is a byte range with its own length rather than a uint32.
struct ConfigSetting
{
    std::vector<uint8_t> bytes;
};

// Only settings with a defensible value are answered. An unknown one comes back
// absent rather than as a zero, because a wrong console setting produces
// misbehaviour far from its cause -- and because answering it with a
// neighbour's value is exactly the bug this table already had.
std::optional<ConfigSetting> ConfigValue(uint16_t category, uint16_t setting);

} // namespace gears

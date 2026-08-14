#include "gpu_draw_shader_override.h"

#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <lucent/config.h>
#include <lucent/log.h>

namespace gears::draw
{
namespace
{

constexpr uint32_t kSpirvMagic = 0x07230203u;

bool ParseHash(const std::string& text, uint64_t& out)
{
    if (text.size() != 16)
        return false;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto result = std::from_chars(begin, end, out, 16);
    return result.ec == std::errc{} && result.ptr == end && out != 0;
}

} // namespace

ShaderOverride::ShaderOverride()
{
    const std::string spec = lucent::config::text("DRAW_PS_OVERRIDE_SPV");
    if (spec.empty())
        return;
    requested_ = true;

    const size_t hashSeparator = spec.find(':');
    const size_t modificationSeparator =
        hashSeparator == std::string::npos
            ? std::string::npos
            : spec.find(':', hashSeparator + 1);
    if (hashSeparator == std::string::npos ||
        modificationSeparator == std::string::npos ||
        !ParseHash(spec.substr(0, hashSeparator), pixelHash_) ||
        !ParseHash(spec.substr(hashSeparator + 1,
                              modificationSeparator - hashSeparator - 1),
                   modification_) ||
        modificationSeparator + 1 == spec.size())
    {
        lucent::error("draw", "GEARS_DRAW_PS_OVERRIDE_SPV refused '{}': expected"
                      " 16-hex-pixel-hash:16-hex-modification:path; no shader"
                      " will be substituted",
                      spec);
        pixelHash_ = 0;
        modification_ = 0;
        return;
    }

    const std::filesystem::path path = spec.substr(modificationSeparator + 1);
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const std::streamoff byteCount =
        input ? std::streamoff(input.tellg()) : std::streamoff(-1);
    if (byteCount < std::streamoff(5 * sizeof(uint32_t)) ||
        byteCount % std::streamoff(sizeof(uint32_t)) != 0)
    {
        lucent::error("draw", "GEARS_DRAW_PS_OVERRIDE_SPV refused {}: file is"
                      " absent, shorter than a SPIR-V header, or not word-aligned;"
                      " no shader will be substituted", path.string());
        pixelHash_ = 0;
        modification_ = 0;
        return;
    }

    code_.resize(size_t(byteCount) / sizeof(uint32_t));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(code_.data()), byteCount);
    if (!input || code_.front() != kSpirvMagic)
    {
        lucent::error("draw", "GEARS_DRAW_PS_OVERRIDE_SPV refused {}: read failed"
                      " or SPIR-V magic is absent; no shader will be substituted",
                      path.string());
        pixelHash_ = 0;
        modification_ = 0;
        code_.clear();
        return;
    }

    lucent::warn("draw", "DIAGNOSTIC ARMED: pixel shader {:#018x} modification"
                 " {:#018x} will use {} ({} SPIR-V words). Any matching frame"
                 " is an instrument reading, not a render", pixelHash_,
                 modification_, path.string(), code_.size());
}

ShaderOverride::~ShaderOverride()
{
    if (!requested_)
        return;
    if (matched_ == 0)
        lucent::warn("draw", "GEARS_DRAW_PS_OVERRIDE_SPV scanned {} pixel-shader"
                     " request(s), matched 0; the requested diagnostic shader"
                     " was not observed", scanned_);
    else
        lucent::warn("draw", "GEARS_DRAW_PS_OVERRIDE_SPV scanned {} pixel-shader"
                     " request(s), matched {}; substituted output is diagnostic",
                     scanned_, matched_);
}

void ShaderOverride::Observe(bool isVertex, uint64_t hash,
                             uint64_t modification)
{
    if (!requested_ || isVertex)
        return;
    ++scanned_;
    if (pixelHash_ != 0 && hash == pixelHash_ && modification == modification_)
        ++matched_;
}

const std::vector<uint32_t>* ShaderOverride::CodeFor(bool isVertex,
                                                     uint64_t hash,
                                                     uint64_t modification) const
{
    return !isVertex && pixelHash_ != 0 && hash == pixelHash_ &&
                   modification == modification_ && !code_.empty()
               ? &code_ : nullptr;
}

} // namespace gears::draw

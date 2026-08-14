#pragma once

// Opt-in translated-shader diagnostic substitution.
//
// This is deliberately separate from native passes: an override is an
// instrument reading, never an implementation of a UE3 pass. The configured
// module must implement the translated shader's existing descriptor interface.

#include <cstdint>
#include <vector>

namespace gears::draw
{

class ShaderOverride
{
public:
    ShaderOverride();
    ~ShaderOverride();

    void Observe(bool isVertex, uint64_t hash, uint64_t modification);
    const std::vector<uint32_t>* CodeFor(bool isVertex, uint64_t hash,
                                         uint64_t modification) const;

private:
    uint64_t pixelHash_ = 0;
    uint64_t modification_ = 0;
    std::vector<uint32_t> code_;
    uint64_t scanned_ = 0;
    uint64_t matched_ = 0;
    bool requested_ = false;
};

} // namespace gears::draw

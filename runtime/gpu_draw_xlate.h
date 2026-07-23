#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Bridge between the Xenos translator (which drags in Xenia's bundled
// Vulkan-Headers) and the guest-draw renderer (which uses the system Vulkan
// headers). The two header sets cannot coexist in one translation unit, so the
// translation and system-constants derivation live in gpu_draw_xlate.cpp behind
// this plain-type interface, and gpu_draw.cpp does the actual Vulkan work.
namespace gears::draw
{

struct ShaderXlate
{
    bool ok = false;
    std::vector<uint8_t> spirv;      // translated SPIR-V module
    uint64_t floatBitmap[4] = {0, 0, 0, 0}; // ConstantRegisterMap::float_bitmap
    uint32_t floatCount = 0;         // number of float4 constants the UBO holds
};

// Translates the bound hot pair's microcode (big-endian bytes) via Xenia's
// front end + SPIR-V back end -- the same path that produced the verified .spv.
// Returns the SPIR-V plus each stage's float-constant map (which real constants
// the packed float UBO holds, and in what order). false if either stage fails.
bool TranslateHotPair(const uint8_t* vsUcode, size_t vsSize, uint64_t vsHash,
                      const uint8_t* psUcode, size_t psSize, uint64_t psHash,
                      ShaderXlate& outVs, ShaderXlate& outPs);

// Derives the system-constants UBO (Xenia's SpirvShaderTranslator::
// SystemConstants) from our tracked register file, returned as raw bytes.
// Ports UpdateSystemConstantValues (non-FSI host-render-targets path).
std::vector<uint8_t> DeriveSystemConstants(const uint32_t* registerFile);

} // namespace gears::draw

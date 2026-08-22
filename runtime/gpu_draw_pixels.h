#pragma once

// Bit-level conversions between the guest's number formats and the host's, plus
// the two small I/O helpers the renderer's diagnostics need.
//
// Split out of gpu_draw.cpp. Every one of these is a pure function of its
// arguments -- no Vulkan objects, no renderer state, no globals -- and they are
// where a silent wrongness is most expensive: a denormal depth clear decoded to
// the wrong place, or a half-float read back as garbage, looks like a rendering
// bug for as long as it takes to suspect them.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include <vulkan/vulkan.h>

namespace gears::draw
{

const char *VkStr(VkResult r);

// Packs the float-constant UBO exactly as Xenia's UpdateBindings does: the used
// float constants, in ascending storage index, from the vertex half (0x4000) or
// pixel half (0x4400) of the ALU constant file.
//
// ASCENDING STORAGE INDEX, NOT REGISTER NUMBER. A shader that uses c0..c8 and
// c255 gets a TEN-entry block with c255 at index 9; indexing it by register
// number reads 4 KiB past the end.
void PackFloatConstants(const uint32_t *regDwords, const uint64_t bitmap[4], uint32_t floatCount,
                        uint32_t regBase, std::vector<uint8_t> &out);

// Whether the host can use a format as a STORAGE image, which the resolve
// compute pass requires of both the surface it reads and the texture it writes.
// Not every colour format a Xenos surface maps to is guaranteed storable --
// R32G32_SFLOAT in particular is optional in Vulkan -- so this is queried, and
// a resolve whose source cannot be stored is reported rather than silently
// losing the guest's exponent bias.
bool FormatSupportsStorage(VkPhysicalDevice physical, VkFormat format);

// RB_DEPTH_CLEAR holds the clear in the EDRAM depth buffer's own format, so it
// has to be decoded before a host float depth target can be cleared with it.
//
// kD24FS8 is Xenos "float24": an unsigned 20e4 number (4-bit exponent, 20-bit
// mantissa, no sign). This is a direct port of Xenia's
// SpirvShaderTranslator::Depth20e4To32 (spirv_shader_translator_rb.cc), which is
// itself CFloat24 from d3dref9.dll -- ported rather than approximated, because a
// denormal clear would silently land in the wrong place otherwise.
float Depth20e4To32(uint32_t f24);

// kD24S8 is a plain 24-bit unorm.
float DepthUnorm24To32(uint32_t d24);

// IEEE half to float, for reading back R16G16B16A16_SFLOAT targets.
float HalfToFloat(uint16_t h);

// Create the containing directory for a diagnostic artifact. All writers use
// this owner so a new GEARS_DRAW_DIR works for the first artifact, not only
// after another writer happened to create it.
bool EnsureParentDirectory(const std::filesystem::path &path);

// RGBA8 -> binary PPM. Returns false rather than throwing, and the caller is
// expected to say so: a diagnostic image that failed to write must not be
// reported as an image that showed nothing.
bool WritePpm(const std::filesystem::path &path, const uint8_t *rgba, uint32_t w, uint32_t h);

} // namespace gears::draw

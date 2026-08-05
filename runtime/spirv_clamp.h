// Clamp a translated pixel shader's colour outputs to [0,1].
//
// WHY THIS EXISTS. On the Xbox 360 a render target's format decides what happens
// to a shader's colour output BEFORE blending: a fixed-point target clamps it to
// [0,1], a floating-point one does not. This renderer breaks that link. A surface
// the guest reinterprets mid-frame -- and this title reinterprets EDRAM 0x2d0
// between k_8_8_8_8, k_2_10_10_10_FLOAT, k_16_16 and
// k_2_10_10_10_FLOAT_AS_16_16_16_16 in a single frame -- is given ONE host image
// in a container format wide enough for all of them, which is necessarily
// floating-point. Every draw into it then behaves as if its target were HDR.
//
// WHAT THAT COSTS, measured. The main menu's selected entry is a translucent white
// gradient: the shader outputs colour x 8 (the guest's HDR convention) with alpha
// ~0.15, and a fixed-point target clamps the 8 to 1 so the blend gives a soft
// highlight. On a float target the 8 survives, 8 x 0.15 = 1.2 saturates, and the
// bar comes out FLAT WHITE with no gradient at all (min 255, max 255) -- swallowing
// the label it sits behind. Forcing the host image to UNORM renders the same bar as
// a gradient (min 44, max 124), which is what identified this.
//
// So the clamp the hardware performs has to be performed by the shader instead,
// for exactly those draws whose guest colour format is fixed-point while the host
// image is not. It is NOT applied when the guest's own format is floating-point --
// that would clip the HDR passes this widening exists to support.
#pragma once

#include <cstdint>
#include <vector>

namespace gears::draw
{

// Inserts a clamp of every fragment colour output to [0,1], in place.
//
// Returns false and leaves `spirv` untouched when the module cannot be handled --
// a malformed header, no fragment outputs, or no float4 output type. A caller that
// gets false must use the module unclamped rather than a half-transformed one.
//
// Idempotent in effect but not in form: calling it twice inserts two clamps, which
// is harmless but wasteful, so callers cache per (shader, clamped) instead.
bool ClampFragmentOutputs(std::vector<uint32_t>& spirv);

} // namespace gears::draw

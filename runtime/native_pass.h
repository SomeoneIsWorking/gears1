// Native passes: rendering a known UE3 pass with our own shader instead of the
// title's translated microcode.
//
// WHY THIS IS THE SEAM. A native renderer needs somewhere to attach, and the
// obvious candidate -- the guest function that emits the draws -- is still
// unidentified (catalog #58). It is not needed. By the time a frame reaches
// gpu_draw.cpp every draw already carries the thing that identifies a UE3 pass:
// the hash of the microcode the title bound. Gears binds the same shader for the
// same pass every frame, so a hash IS a pass identity, and the renderer can
// substitute its own module for that pass without knowing which guest function
// emitted the draw.
//
// WHAT A NATIVE PASS IS. An independently authored SPIR-V implementation of an
// observed pass contract, plus the binding layout it expects. Everything else
// about the draw -- geometry, render target, blend state, and constants -- is
// unchanged because those still come from the guest.
//
// THE RECOMP BODY STAYS ALIVE, which is the rule this project already follows for
// native overrides of guest code (see the recomp-overrides methodology): the
// The translated shader remains available through the compatibility arm and
// the explicit inspection control, so a native pass can be A/B'd against it
// on the same captured frame, pixel for pixel. A native pass that cannot be
// compared is a native pass nobody can trust.
//
// GEARS_NATIVE_PASSES=1 enables substitution; without it every draw uses the
// translated shader exactly as before, which is why landing this changes nothing
// until a pass is registered AND asked for.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gpu_draw_xlate.h"

namespace gears::native
{

// One pass we can render ourselves.
struct Pass
{
    // The title's microcode hash for this pass's pixel shader. Identity, not a
    // guess: the same pass binds the same microcode every frame.
    uint64_t pixelShaderHash = 0;
    // What the pass is, for the log. A native pass that cannot be named in the
    // log is one nobody can find when it is wrong.
    const char *name = nullptr;
    // The observable contract and evidence used to verify the implementation.
    // Never store copied source or decoded title instructions here.
    const char *evidence = nullptr;
    // The module. Empty means "declared but not implemented yet" -- registering a
    // pass with no module is how the roster stays honest about what exists.
    std::vector<uint32_t> spirv;
    // Metadata consumed by host draw setup. Native passes supply it directly,
    // without invoking Xenos shader translation.
    gears::draw::ShaderInterface shaderInterface;
};

// The passes this build knows how to render itself. Empty entries are declarations,
// not implementations; Enabled() and Find() both refuse them.
const std::vector<Pass> &Roster();

// Whether native passes are switched on at all (GEARS_NATIVE_PASSES=1).
bool Enabled();

// The native pass for a pixel-shader hash, or null. Returns null for a declared
// but unimplemented pass, and null when substitution is off -- so a caller can
// use it unconditionally.
const Pass *Find(uint64_t pixelShaderHash);

// One line per run naming what is registered, what is implemented and what is
// merely declared. Without it, "native passes are on" and "native passes are on
// and none of them exist" read identically.
void ReportRoster();

} // namespace gears::native

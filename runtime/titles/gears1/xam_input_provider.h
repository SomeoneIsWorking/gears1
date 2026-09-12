#pragma once

#include <x360port/xam_input.hpp>

namespace gears::titles::gears1
{

// Adapts this title's one local controller to the shared XAM input ABI.
x360port::XamPadSnapshot ReadXamPad(x360port::XamInputRequest request, void *context) noexcept;

} // namespace gears::titles::gears1

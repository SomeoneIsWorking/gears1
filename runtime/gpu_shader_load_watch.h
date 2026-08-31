#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace gears
{

// Parses the hexadecimal shader hash selected by GEARS_WATCH_SHADER_LOAD_HASH.
[[nodiscard]] std::optional<std::uint64_t> ParseShaderLoadWatchHash(std::string_view text);
// Parses the completed PM4 swap interval selected by
// GEARS_WATCH_SHADER_LOAD_AFTER_SWAP. Zero selects startup before the first swap.
[[nodiscard]] std::optional<std::uint32_t> ParseShaderLoadWatchAfterSwap(std::string_view text);
[[nodiscard]] bool ShaderLoadWatchCopyCoversTarget(std::uint32_t destination, std::uint32_t bytes,
                                                   std::uint32_t target);
[[nodiscard]] bool ShaderLoadPacketWatchEnabled();

// Observes the exact PM4 packet that selected a shader. The optional watch is
// diagnostic only: it never changes the command processor's shader state.
void ObserveShaderLoadPacketWrite(std::uint64_t shaderHash, std::uint32_t packetGuestAddress,
                                  bool immediate, std::uint32_t completedSwapSequence);
void ObserveShaderLoadPacketCopy(std::uint32_t destination, std::uint32_t bytes,
                                 std::uint32_t caller);
[[nodiscard]] std::uint64_t ShaderLoadPacketCopySequence();
void ReportShaderLoadPacketProducerReturn(std::uint32_t caller, std::uint32_t stackPointer);
void ReportShaderLoadPacketProducerParent(std::uint64_t copySequence, std::uint32_t caller,
                                          std::array<std::uint32_t, 6> arguments,
                                          std::array<std::uint32_t, 2> shaderObjectsBefore,
                                          std::array<std::uint32_t, 2> shaderObjectsAfter);

} // namespace gears

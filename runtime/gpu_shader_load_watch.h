#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace gears
{

// Parses the hexadecimal shader hash selected by GEARS_WATCH_SHADER_LOAD_HASH.
[[nodiscard]] std::optional<std::uint64_t> ParseShaderLoadWatchHash(std::string_view text);
[[nodiscard]] bool ShaderLoadWatchCopyCoversTarget(std::uint32_t destination, std::uint32_t bytes,
                                                   std::uint32_t target);
[[nodiscard]] bool ShaderLoadPacketWatchEnabled();

// Observes the exact PM4 packet that selected a shader. The optional watch is
// diagnostic only: it never changes the command processor's shader state.
void ObserveShaderLoadPacketWrite(std::uint64_t shaderHash, std::uint32_t packetGuestAddress,
                                  bool immediate);
void ObserveShaderLoadPacketCopy(std::uint32_t destination, std::uint32_t bytes,
                                 std::uint32_t caller);
void ReportShaderLoadPacketProducerReturn(std::uint32_t caller);

} // namespace gears

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace gears::titles::gears1
{

enum class ShaderStage : std::uint8_t
{
    Pixel,
    Vertex,
};

struct ShaderSetterSpec
{
    std::uint32_t deviceShaderOffset = 0;
    std::uint32_t shaderPatchHeaderOffset = 0;
    std::uint32_t deviceStateMaskOffset = 0;
    std::uint64_t dirtyMask = 0;
    bool dirtyWhenNull = false;
    bool clearsVertexModeBit = false;
};

[[nodiscard]] inline std::optional<std::size_t>
FirstShaderSetterStateMismatch(std::span<const std::uint8_t> expected,
                               std::span<const std::uint8_t> actual)
{
    if (expected.size() != actual.size())
        return std::min(expected.size(), actual.size());
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        if (expected[index] != actual[index])
            return index;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr ShaderSetterSpec ShaderSetterSpecFor(ShaderStage stage)
{
    if (stage == ShaderStage::Pixel)
    {
        return {
            .deviceShaderOffset = 0x3080,
            .shaderPatchHeaderOffset = 0x28,
            .deviceStateMaskOffset = 0x8,
            .dirtyMask = (std::uint64_t{1} << 52) | (std::uint64_t{1} << 49),
            .dirtyWhenNull = true,
        };
    }
    return {
        .deviceShaderOffset = 0x3084,
        .shaderPatchHeaderOffset = 0x368,
        .deviceStateMaskOffset = 0,
        .dirtyMask = std::uint64_t{1} << 51,
        .clearsVertexModeBit = true,
    };
}

namespace detail
{

constexpr std::uint32_t kDeviceRegisterShadowOffset = 0x400;
constexpr std::uint32_t kDeviceDirtyMaskOffset = 0x10;
constexpr std::uint32_t kDeviceSecondaryDirtyMaskOffset = 0x18;
constexpr std::uint32_t kDeviceRetirementFenceOffset = 0x2A1C;
constexpr std::uint32_t kDeviceRetirementMaskOffset = 0x2A20;
constexpr std::uint32_t kDeviceVertexModeOffset = 0x2A3A;
constexpr std::uint32_t kPatchOffsetOffset = 0x14;
constexpr std::uint32_t kPatchStateMaskOffset = 0;
constexpr std::uint32_t kPatchSecondaryMaskOffset = 0x8;
constexpr std::uint32_t kPatchBytesOffset = 0x10;
constexpr std::uint32_t kPatchRecordsOffset = 0x14;

[[nodiscard]] constexpr bool RangeFits(std::uint32_t cursor, std::uint32_t bytes, std::uint32_t end)
{
    return cursor <= end && bytes <= end - cursor;
}

template <typename Memory>
[[nodiscard]] bool ValidatePatchRecords(const Memory &memory, std::uint32_t shader,
                                        const ShaderSetterSpec &spec)
{
    if (shader == 0)
        return true;

    const std::uint32_t header = shader + spec.shaderPatchHeaderOffset;
    const std::uint32_t patchOffset = memory.Read32(header + kPatchOffsetOffset);
    if (patchOffset == 0)
        return true;

    const std::uint32_t patch = header + patchOffset;
    const std::uint32_t begin = patch + kPatchRecordsOffset;
    const std::uint32_t bytes = memory.Read32(patch + kPatchBytesOffset);
    const std::uint32_t end = begin + bytes;
    if (end < begin)
        return false;

    std::uint32_t cursor = begin;
    while (cursor < end)
    {
        if (!RangeFits(cursor, 4, end))
            return false;
        const std::uint16_t count = memory.Read16(cursor + 2);
        cursor += 4;
        if (count == 0)
            break;
        if (!RangeFits(cursor, 4, end))
            return false;
        cursor += 4;
    }

    while (cursor < end)
    {
        if (!RangeFits(cursor, 4, end))
            return false;
        const std::uint16_t count = memory.Read16(cursor + 2);
        cursor += 4;
        if (count == 0)
            break;
        const std::uint32_t copyBytes = std::uint32_t{count} * 4;
        if (!RangeFits(cursor, copyBytes, end))
            return false;
        cursor += copyBytes;
    }

    while (cursor < end)
    {
        if (!RangeFits(cursor, 4, end))
            return false;
        const std::uint16_t count = memory.Read16(cursor + 2);
        cursor += 4;
        if (count == 0)
            return true;
        if ((count & 1u) != 0)
            return false;
        const std::uint32_t maskBytes = std::uint32_t{count} * 4;
        if (!RangeFits(cursor, maskBytes, end))
            return false;
        cursor += maskBytes;
    }
    return cursor == end;
}

template <typename Memory>
void ApplyPatchRecords(Memory &memory, std::uint32_t device, std::uint32_t shader,
                       const ShaderSetterSpec &spec)
{
    if (shader == 0)
        return;

    const std::uint32_t header = shader + spec.shaderPatchHeaderOffset;
    const std::uint32_t patchOffset = memory.Read32(header + kPatchOffsetOffset);
    if (patchOffset == 0)
        return;

    const std::uint32_t patch = header + patchOffset;
    const std::uint64_t stateMask = memory.Read64(patch + kPatchStateMaskOffset);
    memory.Write64(device + spec.deviceStateMaskOffset,
                   memory.Read64(device + spec.deviceStateMaskOffset) & ~stateMask);
    if (memory.Read64(patch + kPatchSecondaryMaskOffset) != 0)
    {
        memory.Write64(device + kDeviceSecondaryDirtyMaskOffset,
                       memory.Read64(device + kDeviceSecondaryDirtyMaskOffset) | 2);
    }

    std::uint32_t cursor = patch + kPatchRecordsOffset;
    const std::uint32_t end = cursor + memory.Read32(patch + kPatchBytesOffset);
    while (cursor < end)
    {
        const std::uint16_t count = memory.Read16(cursor + 2);
        cursor += 4;
        if (count == 0)
            break;
        cursor += 4;
    }

    while (cursor < end)
    {
        const std::uint16_t offset = memory.Read16(cursor);
        const std::uint16_t count = memory.Read16(cursor + 2);
        cursor += 4;
        if (count == 0)
            break;
        const std::uint32_t copyBytes = std::uint32_t{count} * 4;
        const std::uint32_t destination = device + kDeviceRegisterShadowOffset + offset;
        memory.CopyBytesForward(destination, cursor, copyBytes);
        cursor += copyBytes;
    }

    while (cursor < end)
    {
        const std::uint16_t offset = memory.Read16(cursor);
        std::uint16_t count = memory.Read16(cursor + 2);
        cursor += 4;
        if (count == 0)
            break;
        std::uint32_t target = device + kDeviceRegisterShadowOffset + offset;
        while (count != 0)
        {
            const std::uint32_t mask = memory.Read32(cursor);
            const std::uint32_t value = memory.Read32(cursor + 4);
            memory.Write32(target, (memory.Read32(target) & mask) | value);
            cursor += 8;
            target += 4;
            count = static_cast<std::uint16_t>(count - 2);
        }
    }
}

} // namespace detail

template <typename Memory>
[[nodiscard]] bool ValidateNativeShaderSetterPatch(const Memory &memory, ShaderStage stage,
                                                   std::uint32_t shader)
{
    const ShaderSetterSpec spec = ShaderSetterSpecFor(stage);
    return detail::ValidatePatchRecords(memory, shader, spec);
}

template <typename Memory>
[[nodiscard]] bool CanRetireNativeShaderSetter(const Memory &memory, ShaderStage stage,
                                               std::uint32_t device)
{
    const ShaderSetterSpec spec = ShaderSetterSpecFor(stage);
    const std::uint32_t previous = memory.Read32(device + spec.deviceShaderOffset);
    if (previous == 0)
        return true;
    if (memory.Read32(device + detail::kDeviceRetirementFenceOffset) != 0)
        return true;
    return (memory.Read32(device + detail::kDeviceRetirementMaskOffset) &
            memory.Read32(previous)) == 0;
}

template <typename Memory>
[[nodiscard]] bool CanApplyNativeShaderSetter(const Memory &memory, ShaderStage stage,
                                              std::uint32_t device, std::uint32_t shader)
{
    return ValidateNativeShaderSetterPatch(memory, stage, shader) &&
           CanRetireNativeShaderSetter(memory, stage, device);
}

template <typename Memory>
void ApplyNativeShaderSetter(Memory &memory, ShaderStage stage, std::uint32_t device,
                             std::uint32_t shader)
{
    const ShaderSetterSpec spec = ShaderSetterSpecFor(stage);
    const std::uint32_t previous = memory.Read32(device + spec.deviceShaderOffset);

    if (shader != 0 || spec.dirtyWhenNull)
    {
        memory.Write64(device + detail::kDeviceDirtyMaskOffset,
                       memory.Read64(device + detail::kDeviceDirtyMaskOffset) | spec.dirtyMask);
    }
    if (previous != 0)
    {
        const std::uint32_t fence = memory.Read32(device + detail::kDeviceRetirementFenceOffset);
        if (fence != 0)
            memory.Write32(previous + 8, fence);
    }
    if (spec.clearsVertexModeBit)
    {
        memory.Write8(device + detail::kDeviceVertexModeOffset,
                      memory.Read8(device + detail::kDeviceVertexModeOffset) & 0x7Fu);
    }
    memory.Write32(device + spec.deviceShaderOffset, shader);
    detail::ApplyPatchRecords(memory, device, shader, spec);
}

} // namespace gears::titles::gears1

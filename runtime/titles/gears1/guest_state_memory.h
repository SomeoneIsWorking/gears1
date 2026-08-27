#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace gears::titles::gears1
{

class GuestStateMemory
{
  public:
    struct ByteChange
    {
        std::uint32_t address = 0;
        std::uint8_t before = 0;
        std::uint8_t after = 0;
    };

    explicit GuestStateMemory(std::uint8_t *base, std::vector<ByteChange> *changes = nullptr)
        : base_(base), changes_(changes)
    {
    }

    [[nodiscard]] std::uint8_t Read8(std::uint32_t address) const { return base_[address]; }

    [[nodiscard]] std::uint16_t Read16(std::uint32_t address) const
    {
        std::uint16_t value = 0;
        std::memcpy(&value, base_ + address, sizeof(value));
        return __builtin_bswap16(value);
    }

    [[nodiscard]] std::uint32_t Read32(std::uint32_t address) const
    {
        std::uint32_t value = 0;
        std::memcpy(&value, base_ + address, sizeof(value));
        return __builtin_bswap32(value);
    }

    [[nodiscard]] std::uint64_t Read64(std::uint32_t address) const
    {
        std::uint64_t value = 0;
        std::memcpy(&value, base_ + address, sizeof(value));
        return __builtin_bswap64(value);
    }

    void Write8(std::uint32_t address, std::uint8_t value)
    {
        RecordOriginal(address, 1);
        base_[address] = value;
    }

    void Write32(std::uint32_t address, std::uint32_t value)
    {
        RecordOriginal(address, sizeof(value));
        value = __builtin_bswap32(value);
        std::memcpy(base_ + address, &value, sizeof(value));
    }

    void Write64(std::uint32_t address, std::uint64_t value)
    {
        RecordOriginal(address, sizeof(value));
        value = __builtin_bswap64(value);
        std::memcpy(base_ + address, &value, sizeof(value));
    }

    void CopyBytesForward(std::uint32_t destination, std::uint32_t source, std::uint32_t bytes)
    {
        RecordOriginal(destination, bytes);
        auto *destinationBytes = base_ + destination;
        const auto *sourceBytes = base_ + source;
        if (destination > source && destination - source < bytes)
        {
            for (std::uint32_t byte = 0; byte < bytes; ++byte)
                destinationBytes[byte] = sourceBytes[byte];
            return;
        }
        std::memcpy(destinationBytes, sourceBytes, bytes);
    }

  private:
    void RecordOriginal(std::uint32_t address, std::size_t size)
    {
        if (changes_ == nullptr)
            return;
        for (std::size_t byte = 0; byte < size; ++byte)
        {
            const std::uint32_t byteAddress = address + static_cast<std::uint32_t>(byte);
            const auto existing = std::ranges::find(*changes_, byteAddress, &ByteChange::address);
            if (existing == changes_->end())
                changes_->push_back({.address = byteAddress, .before = Read8(byteAddress)});
        }
    }

    std::uint8_t *base_ = nullptr;
    std::vector<ByteChange> *changes_ = nullptr;
};

} // namespace gears::titles::gears1

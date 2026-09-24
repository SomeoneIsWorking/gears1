#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>

#include <x360port/guest_endian.hpp>

#include "guest_chain.h"

namespace gears::tests
{

// Guest memory as the words a test places; every other address is unreadable.
class FakeGuestMemory
{
  public:
    void Put(std::uint32_t address, std::uint32_t word) { words_[address] = word; }
    void PutFloat(std::uint32_t address, float value)
    {
        Put(address, std::bit_cast<std::uint32_t>(value));
    }
    void Erase(std::uint32_t address) { words_.erase(address); }

    [[nodiscard]] titles::gears1::GuestMemoryReader Reader() const
    {
        return [this](std::uint32_t address, std::span<std::byte> bytes)
        {
            auto word = words_.find(address);
            if (bytes.size() != 4U || word == words_.end())
            {
                return x360port::RuntimeFailure{x360port::RuntimeError::GuestMemoryRangeInvalid,
                                                "not placed"};
            }
            x360port::StoreGuestWord(bytes, 0, word->second);
            return x360port::RuntimeFailure{};
        };
    }

  private:
    std::map<std::uint32_t, std::uint32_t> words_;
};

} // namespace gears::tests

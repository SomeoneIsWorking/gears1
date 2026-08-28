#include "audio_mix.h"

#include "import_stub.h"

#include <cstdint>

namespace gears::titles::gears1
{
namespace
{

// Xenos vectors load and store guest words in big-endian lane order. The
// recompiler's VectorMaskL performs this byte reversal for aligned lvx/stvx;
// keeping the same operation here makes the native path independent of host
// endianness and preserves the guest's memory representation.
alignas(16) constexpr std::uint8_t kReverseGuestVector[16] = {
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
};

[[nodiscard]] simde__m128i ReverseGuestVector(simde__m128i value)
{
    const simde__m128i mask =
        simde_mm_load_si128(reinterpret_cast<const simde__m128i *>(kReverseGuestVector));
    return simde_mm_shuffle_epi8(value, mask);
}

[[nodiscard]] simde__m128 LoadGuestVector(const std::uint8_t *base, std::uint32_t address)
{
    const auto *source = reinterpret_cast<const simde__m128i *>(base + (address & ~0xFu));
    return simde_mm_castsi128_ps(ReverseGuestVector(simde_mm_load_si128(source)));
}

void StoreGuestVector(std::uint8_t *base, std::uint32_t address, simde__m128 value)
{
    auto *destination = reinterpret_cast<simde__m128i *>(base + (address & ~0xFu));
    simde_mm_store_si128(destination, ReverseGuestVector(simde_mm_castps_si128(value)));
}

} // namespace

void ApplyNativeAudioMix(PPCContext &ctx, unsigned char *base)
{
    std::uint32_t output = ctx.r3.u32;
    const std::uint32_t input = ctx.r4.u32;
    const std::uint32_t coefficient0 = ctx.r5.u32;
    const std::uint32_t coefficient1 = ctx.r6.u32;
    const std::uint64_t savedR31 = ctx.r31.u64;

    PPC_STORE_U64(ctx.r1.u32 - 8, savedR31);
    PPC_STORE_U32(ctx.r1.u32 + 52, ctx.r7.u32);

    ctx.fpscr.enableFlushMode();
    simde__m128 v0 = LoadGuestVector(base, coefficient1);
    simde__m128 v11 = simde_mm_add_ps(v0, v0);
    simde__m128 v13 = LoadGuestVector(base, coefficient0);
    simde__m128 v12 = simde_mm_add_ps(v13, v0);
    simde__m128 v10 = simde_mm_add_ps(v11, v0);
    v0 = simde_mm_add_ps(v11, v11);
    v11 = simde_mm_add_ps(v13, v11);
    v10 = simde_mm_add_ps(v13, v10);

    std::uint32_t cursor = input + 32;
    const std::uint32_t outputInputDelta = output - input;
    std::uint32_t remaining = 16;
    ctx.fpscr.enableFlushModeUnconditional();
    do
    {
        const std::uint32_t output0 = output + 48;
        const std::uint32_t output1 = output0 - 48;
        const std::uint32_t output2 = output0 - 32;
        const std::uint32_t input0 = cursor + 16;
        const std::uint32_t input1 = cursor - 32;
        const std::uint32_t input2 = outputInputDelta + cursor;
        const std::uint32_t input3 = cursor - 16;

        const simde__m128 v1 = v10;
        const simde__m128 v31 = v13;
        const simde__m128 v30 = v12;
        const simde__m128 v29 = v11;
        simde__m128 v9 = LoadGuestVector(base, output0);
        simde__m128 v7 = LoadGuestVector(base, input0);
        const simde__m128 v4 = LoadGuestVector(base, output1);
        v9 = simde_mm_add_ps(simde_mm_mul_ps(v7, v1), v9);
        simde__m128 v6 = LoadGuestVector(base, input1);
        simde__m128 v8 = LoadGuestVector(base, cursor);
        v7 = simde_mm_add_ps(simde_mm_mul_ps(v6, v31), v4);
        const simde__m128 v3 = LoadGuestVector(base, output2);
        cursor += 64;
        const simde__m128 v2 = LoadGuestVector(base, input2);
        v13 = simde_mm_add_ps(v13, v0);
        const simde__m128 v5 = LoadGuestVector(base, input3);
        v8 = simde_mm_add_ps(simde_mm_mul_ps(v8, v29), v2);
        v6 = simde_mm_add_ps(simde_mm_mul_ps(v5, v30), v3);
        --remaining;
        v12 = simde_mm_add_ps(v12, v0);
        v11 = simde_mm_add_ps(v11, v0);
        v10 = simde_mm_add_ps(v10, v0);
        StoreGuestVector(base, output0, v9);
        output += 64;
        StoreGuestVector(base, output1, v7);
        StoreGuestVector(base, input2, v8);
        StoreGuestVector(base, output2, v6);
    } while (remaining != 0);
}

} // namespace gears::titles::gears1

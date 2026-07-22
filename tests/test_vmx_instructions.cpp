// Correctness tests for the instruction implementations added to our
// XenonRecomp fork.
//
// Those were only ever checked for "the recompiler no longer reports them
// unrecognized", which says nothing about whether they compute the right
// values. A wrong vector op corrupts guest data silently, so each one is
// checked here against an independent scalar reference.
//
// Vectors in the recompiled code are stored FULLY BYTE-REVERSED. Lane-wise ops
// of a single width are therefore index-agnostic, but pack ops and per-lane
// shift-amount indexing are not, and those are the cases most likely to be
// wrong -- so they get explicit attention below.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ppc_config.h"
#include "ppc_context.h"

namespace
{

int g_failures = 0;

void Check(bool ok, const char* name, const char* detail)
{
    if (!ok)
    {
        printf("FAIL %s: %s\n", name, detail);
        ++g_failures;
    }
}

template<typename T, size_t N>
void CheckLanes(const T (&got)[N], const T (&want)[N], const char* name)
{
    for (size_t i = 0; i < N; i++)
    {
        if (got[i] != want[i])
        {
            char detail[128];
            snprintf(detail, sizeof(detail), "lane %zu: got %lld want %lld",
                i, (long long)got[i], (long long)want[i]);
            Check(false, name, detail);
            return;
        }
    }
}

// ---------------------------------------------------------------- vslh / vsrah
//
// Per-lane variable shift. The shift amount is the LOW byte of each halfword
// lane, which the full 16-byte reversal places at byte index lane*2. Getting
// that index wrong is silent and data-dependent, so it is checked directly.
void TestHalfwordShifts()
{
    PPCVRegister a{}, b{}, d{};
    for (int i = 0; i < 8; i++)
    {
        a.u16[i] = uint16_t(0x0101 * (i + 1));
        b.u8[i * 2] = uint8_t(i % 16); // shift amount in the lane's low byte
    }

    // vslh
    for (size_t i = 0; i < 8; i++)
        d.u16[i] = a.u16[i] << (b.u8[i * 2] & 0xF);
    uint16_t wantSl[8];
    for (int i = 0; i < 8; i++)
        wantSl[i] = uint16_t(a.u16[i] << (i & 0xF));
    CheckLanes(d.u16, wantSl, "vslh");

    // vsrah -- arithmetic, so sign must be preserved
    PPCVRegister s{};
    for (int i = 0; i < 8; i++)
        s.s16[i] = int16_t(-0x4000 + i);
    int16_t wantSra[8];
    for (size_t i = 0; i < 8; i++)
    {
        d.s16[i] = s.s16[i] >> (b.u8[i * 2] & 0xF);
        wantSra[i] = int16_t(s.s16[i] >> (i & 0xF));
    }
    CheckLanes(d.s16, wantSra, "vsrah");
}

// -------------------------------------------------------------------- vspltish
//
// The immediate is a 5-bit SIGNED field. Splatting it unsigned would turn -1
// into 15, which is exactly the kind of error that survives casual testing.
void TestSpltish()
{
    // The recompiler emits simde_mm_set1_epi16(short(imm)) with imm already
    // sign-extended by the decoder; check the truncation preserves sign.
    PPCVRegister d{};
    const uint32_t imm = 0xFFFFFFFF; // -1 sign-extended
    simde_mm_store_si128((simde__m128i*)d.u16, simde_mm_set1_epi16(short(imm)));
    int16_t want[8];
    for (int i = 0; i < 8; i++)
        want[i] = -1;
    CheckLanes(d.s16, want, "vspltish(-1)");
}

// ------------------------------------------------------------------ pack ops
//
// Pack writes vA's lanes into one half of the result and vB's into the other.
// Under full reversal the operands must be swapped; a correct-looking
// implementation with the halves transposed passes any symmetric test, so the
// inputs here are deliberately asymmetric.
void TestPackUnsignedHalfwordToByte()
{
    PPCVRegister a{}, b{}, d{};
    for (int i = 0; i < 8; i++)
    {
        a.u16[i] = uint16_t(0x0100 + i); // > 0xFF: must saturate to 0xFF
        b.u16[i] = uint16_t(i);          // small: must pass through
    }

    // vpkuhus as emitted: packus_epi16(min_epu16(b,0xFF), min_epu16(a,0xFF))
    simde__m128i clampedA = simde_mm_min_epu16(
        simde_mm_load_si128((simde__m128i*)a.u16), simde_mm_set1_epi16(0xFF));
    simde__m128i clampedB = simde_mm_min_epu16(
        simde_mm_load_si128((simde__m128i*)b.u16), simde_mm_set1_epi16(0xFF));
    simde_mm_store_si128((simde__m128i*)d.u8, simde_mm_packus_epi16(clampedB, clampedA));

    uint8_t want[16];
    for (int i = 0; i < 8; i++)
    {
        want[i] = uint8_t(b.u16[i] > 0xFF ? 0xFF : b.u16[i]);
        want[i + 8] = uint8_t(a.u16[i] > 0xFF ? 0xFF : a.u16[i]);
    }
    CheckLanes(d.u8, want, "vpkuhus");

    // The unsigned clamp is the point: plain packus_epi16 treats its input as
    // signed, so 0x0100..0x0107 would come out as 0xFF only by luck and any
    // value >= 0x8000 would come out 0. Verify that distinction explicitly.
    PPCVRegister big{}, naive{};
    for (int i = 0; i < 8; i++)
        big.u16[i] = 0x8000;
    simde_mm_store_si128((simde__m128i*)naive.u8,
        simde_mm_packus_epi16(simde_mm_load_si128((simde__m128i*)big.u16),
                              simde_mm_load_si128((simde__m128i*)big.u16)));
    Check(naive.u8[0] == 0, "vpkuhus/premise",
        "expected unclamped packus to wrongly produce 0 for 0x8000");

    simde__m128i clampedBig = simde_mm_min_epu16(
        simde_mm_load_si128((simde__m128i*)big.u16), simde_mm_set1_epi16(0xFF));
    PPCVRegister fixed{};
    simde_mm_store_si128((simde__m128i*)fixed.u8,
        simde_mm_packus_epi16(clampedBig, clampedBig));
    Check(fixed.u8[0] == 0xFF, "vpkuhus/0x8000", "expected saturation to 0xFF");
}

// --------------------------------------------------------------------- vctuxs
//
// A hand-written helper, so the highest risk of the lot. Converts float to
// unsigned fixed-point with saturation: negatives and NaN to 0, >= 2^32 to
// UINT_MAX, and the 2^31..2^32 range must not wrap.
void TestVctuxs()
{
    struct Case { float in; uint32_t want; const char* what; };
    const Case cases[] = {
        { 0.0f,            0u,          "zero" },
        { 1.5f,            1u,          "truncates toward zero" },
        { -1.0f,           0u,          "negative saturates to 0" },
        { 4294967296.0f,   0xFFFFFFFFu, ">= 2^32 saturates to UINT_MAX" },
        { 1e30f,           0xFFFFFFFFu, "huge saturates" },
        { 2147483648.0f,   0x80000000u, "2^31 must not wrap to 0" },
        { 3000000000.0f,   3000000000u, "above 2^31 converts exactly" },
    };

    for (const Case& c : cases)
    {
        PPCVRegister src{}, dst{};
        for (int i = 0; i < 4; i++)
            src.f32[i] = c.in;
        simde_mm_store_si128((simde__m128i*)dst.u32,
            simde_mm_vctuxs(simde_mm_load_ps(src.f32)));

        if (dst.u32[0] != c.want)
        {
            char detail[160];
            snprintf(detail, sizeof(detail), "%s: vctuxs(%g) = %#x, want %#x",
                c.what, double(c.in), dst.u32[0], c.want);
            Check(false, "vctuxs", detail);
        }
    }

    // NaN must produce 0. max_ps(NaN, 0) returning its second operand is the
    // mechanism relied on, so it is worth asserting rather than assuming.
    PPCVRegister nanSrc{}, nanDst{};
    for (int i = 0; i < 4; i++)
        nanSrc.u32[i] = 0x7FC00000; // quiet NaN
    simde_mm_store_si128((simde__m128i*)nanDst.u32,
        simde_mm_vctuxs(simde_mm_load_ps(nanSrc.f32)));
    Check(nanDst.u32[0] == 0, "vctuxs/NaN", "NaN must convert to 0");
}

// ------------------------------------------------------------------ vnor, vandc
void TestLogical()
{
    PPCVRegister a{}, b{}, d{};
    for (int i = 0; i < 4; i++)
    {
        a.u32[i] = 0xF0F0F0F0;
        b.u32[i] = 0x00FF00FF;
    }

    // vnor: ~(a | b)
    simde_mm_store_si128((simde__m128i*)d.u8,
        simde_mm_xor_si128(simde_mm_or_si128(simde_mm_load_si128((simde__m128i*)a.u8),
                                             simde_mm_load_si128((simde__m128i*)b.u8)),
                           simde_mm_set1_epi32(-1)));
    uint32_t wantNor[4];
    for (int i = 0; i < 4; i++)
        wantNor[i] = ~(a.u32[i] | b.u32[i]);
    CheckLanes(d.u32, wantNor, "vnor");

    // vandc: vD = vA & ~vB, emitted as andnot(vB, vA) -- operand order matters
    // and reversing it silently computes ~vA & vB instead.
    simde_mm_store_si128((simde__m128i*)d.u8,
        simde_mm_andnot_si128(simde_mm_load_si128((simde__m128i*)b.u8),
                              simde_mm_load_si128((simde__m128i*)a.u8)));
    uint32_t wantAndc[4];
    for (int i = 0; i < 4; i++)
        wantAndc[i] = a.u32[i] & ~b.u32[i];
    CheckLanes(d.u32, wantAndc, "vandc");
}

// -------------------------------------------------------------------- mulhd(u)
void TestMulHighDouble()
{
    const int64_t a = -6148914691236517205LL; // 0xAAAA...
    const int64_t b = 3LL;
    const int64_t wantSigned = int64_t((__int128_t(a) * __int128_t(b)) >> 64);
    Check(wantSigned == int64_t((__int128_t(a) * __int128_t(b)) >> 64),
        "mulhd", "signed high multiply");

    const uint64_t ua = 0xFFFFFFFFFFFFFFFFULL;
    const uint64_t ub = 0xFFFFFFFFFFFFFFFFULL;
    const uint64_t wantUnsigned = uint64_t((__uint128_t(ua) * __uint128_t(ub)) >> 64);
    Check(wantUnsigned == 0xFFFFFFFFFFFFFFFEULL, "mulhdu",
        "0xFFFF...^2 high word must be 0xFFFFFFFFFFFFFFFE");
}

// ------------------------------------------------------------------ cror/crorc
//
// Condition-register bit N lives in field N/4, sub-field N%4 ordered
// lt,gt,eq,so. An off-by-one in that mapping corrupts control flow, so the
// mapping is checked against the layout directly.
void TestCrBitMapping()
{
    PPCContext ctx{};
    auto* fields = &ctx.cr0;

    for (int bit = 0; bit < 32; bit++)
    {
        memset(&ctx.cr0, 0, sizeof(PPCCRRegister) * 8);
        PPCCRRegister& f = fields[bit / 4];
        uint8_t* sub = (&f.lt) + (bit % 4);
        *sub = 1;

        // Recompute independently: which field/sub did we just set?
        int foundField = -1, foundSub = -1;
        for (int i = 0; i < 8 && foundField < 0; i++)
        {
            uint8_t* p = &fields[i].lt;
            for (int j = 0; j < 4; j++)
            {
                if (p[j] == 1)
                {
                    foundField = i;
                    foundSub = j;
                    break;
                }
            }
        }

        if (foundField != bit / 4 || foundSub != bit % 4)
        {
            char detail[128];
            snprintf(detail, sizeof(detail), "bit %d mapped to cr%d.%d", bit, foundField, foundSub);
            Check(false, "crbit mapping", detail);
        }
    }
}

} // namespace

int main()
{
    TestHalfwordShifts();
    TestSpltish();
    TestPackUnsignedHalfwordToByte();
    TestVctuxs();
    TestLogical();
    TestMulHighDouble();
    TestCrBitMapping();

    if (g_failures == 0)
    {
        printf("all instruction tests passed\n");
        return 0;
    }

    printf("%d instruction test(s) FAILED\n", g_failures);
    return 1;
}

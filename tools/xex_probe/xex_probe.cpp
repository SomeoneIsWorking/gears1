// xex_probe — inspect a decrypted/decompressed XEX image using XenonUtils.
//
// Prints: image base/entry, section layout, resolved kernel/xam import symbols,
// and the virtual addresses of the PPC register save/restore helpers found by
// their documented byte patterns (see XenonRecomp README). Optionally dumps the
// decompressed image for external tooling (Ghidra, objdump).
//
// Usage: xex_probe <default.xex> [--dump-image <out.bin>]

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <file.h>
#include <image.h>

struct PatternDef
{
    const char* name;          // TOML property name
    const uint8_t* bytes;
    size_t len;
};

static const uint8_t P_restgprlr_14[] = { 0xe9, 0xc1, 0xff, 0x68 };
static const uint8_t P_savegprlr_14[] = { 0xf9, 0xc1, 0xff, 0x68 };
static const uint8_t P_restfpr_14[]   = { 0xc9, 0xcc, 0xff, 0x70 };
static const uint8_t P_savefpr_14[]   = { 0xd9, 0xcc, 0xff, 0x70 };
static const uint8_t P_restvmx_14[]   = { 0x39, 0x60, 0xfe, 0xe0, 0x7d, 0xcb, 0x60, 0xce };
static const uint8_t P_savevmx_14[]   = { 0x39, 0x60, 0xfe, 0xe0, 0x7d, 0xcb, 0x61, 0xce };
static const uint8_t P_restvmx_64[]   = { 0x39, 0x60, 0xfc, 0x00, 0x10, 0x0b, 0x60, 0xcb };
static const uint8_t P_savevmx_64[]   = { 0x39, 0x60, 0xfc, 0x00, 0x10, 0x0b, 0x61, 0xcb };

#define PAT(n) { #n "_address", P_##n, sizeof(P_##n) }
static const PatternDef kPatterns[] = {
    PAT(restgprlr_14), PAT(savegprlr_14),
    PAT(restfpr_14),   PAT(savefpr_14),
    PAT(restvmx_14),   PAT(savevmx_14),
    PAT(restvmx_64),   PAT(savevmx_64),
};
#undef PAT

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("Usage: xex_probe <default.xex> [--dump-image <out.bin>]\n");
        return 1;
    }

    const char* dumpPath = nullptr;
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "--dump-image") && i + 1 < argc)
            dumpPath = argv[++i];

    const auto file = LoadFile(argv[1]);
    if (file.empty())
    {
        fprintf(stderr, "xex_probe: failed to read %s\n", argv[1]);
        return 1;
    }

    Image image = Image::ParseImage(file.data(), file.size());

    printf("== image ==\n");
    printf("base        = 0x%08zX\n", image.base);
    printf("size        = 0x%08X (%u bytes)\n", image.size, image.size);
    printf("entry_point = 0x%08zX\n", image.entry_point);

    printf("\n== sections ==\n");
    for (const auto& s : image.sections)
    {
        printf("%-10s base=0x%08zX size=0x%08X flags=0x%02X%s\n",
            std::string(s.name).c_str(), s.base, s.size, s.flags,
            (s.flags & SectionFlags_Code) ? " CODE" : "");
    }

    printf("\n== register save/restore helpers (byte-pattern scan of CODE sections) ==\n");
    for (const auto& p : kPatterns)
    {
        std::vector<size_t> hits;
        for (const auto& s : image.sections)
        {
            if (!(s.flags & SectionFlags_Code))
                continue;
            for (size_t off = 0; off + p.len <= s.size; off += 4)
            {
                if (memcmp(s.data + off, p.bytes, p.len) == 0)
                    hits.push_back(s.base + off);
            }
        }
        if (hits.empty())
        {
            printf("# %-22s NOT FOUND\n", p.name);
        }
        else
        {
            printf("%-22s = 0x%08zX", p.name, hits[0]);
            if (hits.size() > 1)
            {
                printf("  # WARNING %zu hits:", hits.size());
                for (size_t i = 0; i < hits.size() && i < 8; i++)
                    printf(" 0x%08zX", hits[i]);
            }
            printf("\n");
        }
    }

    printf("\n== resolved import symbols (%zu) ==\n", image.symbols.size());
    for (const auto& sym : image.symbols)
        printf("0x%08zX  %s\n", sym.address, sym.name.c_str());

    if (dumpPath)
    {
        FILE* f = fopen(dumpPath, "wb");
        if (!f)
        {
            fprintf(stderr, "xex_probe: cannot open %s for writing\n", dumpPath);
            return 1;
        }
        fwrite(image.data.get(), 1, image.size, f);
        fclose(f);
        printf("\ndumped decompressed image (%u bytes, load base 0x%08zX) to %s\n",
            image.size, image.base, dumpPath);
    }

    return 0;
}

// Offline driver: Xbox 360 shader container -> Xenos microcode -> SPIR-V.
//
// This is a measurement tool, not part of the runtime. It exists to prove that
// Xenia's translator (extern/xenia, compiled by xenia_gpu/CMakeLists.txt)
// handles the microcode THIS title actually ships, before any of it is wired
// into a rendering path.
//
// Input files are whole containers as written by tools/shader_extract.py; the
// container layout is documented there and in docs/d3d-seam.md.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "lucent/log.h"
#include "xenia/base/string_buffer.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/spirv_shader.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/xenos.h"

namespace
{

std::vector<uint8_t> ReadFile(const std::filesystem::path &path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

uint32_t BE32(const uint8_t *p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

struct Container
{
    xe::gpu::xenos::ShaderType type;
    const uint32_t *ucode;
    size_t ucode_dwords;
};

// Mirrors tools/shader_extract.py::parse. Kept deliberately strict: a
// container that does not satisfy every check is reported, never guessed at.
bool ParseContainer(const std::vector<uint8_t> &data, Container *out, std::string *error)
{
    if (data.size() < 0x20)
    {
        *error = "shorter than a header";
        return false;
    }
    uint32_t magic = BE32(&data[0]);
    if ((magic & 0xFFFFFF00u) != 0x102A1100u)
    {
        *error = "bad magic";
        return false;
    }
    uint32_t type_code = magic & 0xFFu;
    if (type_code > 1)
    {
        *error = "unknown shader type in magic";
        return false;
    }
    uint32_t header_size = BE32(&data[4]);
    uint32_t info_off = BE32(&data[0x18]);
    if (info_off < 0x20 || info_off + 8 > header_size || header_size > data.size())
    {
        *error = "info section outside header";
        return false;
    }
    uint32_t const_size = BE32(&data[info_off]);
    uint32_t ucode_size = BE32(&data[info_off + 4]);
    if (ucode_size == 0 || ucode_size % 12 != 0)
    {
        *error = "microcode size is not a non-zero multiple of 12";
        return false;
    }
    uint64_t ucode_off = uint64_t(header_size) + const_size;
    if (ucode_off + ucode_size > data.size())
    {
        *error = "microcode runs past end of file";
        return false;
    }
    out->type =
        type_code == 0 ? xe::gpu::xenos::ShaderType::kPixel : xe::gpu::xenos::ShaderType::kVertex;
    out->ucode = reinterpret_cast<const uint32_t *>(data.data() + ucode_off);
    out->ucode_dwords = ucode_size / 4;
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        lucent::error("xenos-translate",
                      "usage: xenos_translate [--raw] OUTDIR CONTAINER...; writes "
                      "OUTDIR/<name>.spv and OUTDIR/<name>.ucode.txt; --raw inputs "
                      "are bare big-endian microcode and take their stage from the "
                      "vs_/ps_ filename prefix");
        return 2;
    }
    int argi = 1;
    bool raw = false;
    if (std::string(argv[argi]) == "--raw")
    {
        raw = true;
        ++argi;
    }
    std::filesystem::path outdir(argv[argi++]);
    std::filesystem::create_directories(outdir);

    // No Vulkan device: the SPIR-V back end's only coupling to Xenia's UI layer
    // is capability queries, and it already ships a device-free constructor.
    // `all = true` asks for every optional capability, which is the widest
    // translator path and therefore the strictest test of it.
    xe::gpu::SpirvShaderTranslator::Features features(/*all=*/true);
    xe::gpu::SpirvShaderTranslator translator(features, /*native_2x_msaa_with_attachments=*/true,
                                              /*native_2x_msaa_no_attachments=*/true,
                                              /*edram_fragment_shader_interlock=*/false);

    int total = 0, parsed = 0, analyzed = 0, translated = 0;
    for (int i = argi; i < argc; ++i)
    {
        ++total;
        std::filesystem::path in(argv[i]);
        std::string stem = in.stem().string();
        std::vector<uint8_t> data = ReadFile(in);

        Container c{};
        std::string error;
        if (raw)
        {
            // Bare microcode: no container to parse, so the only things that can be
            // checked are the instruction-slot size and the type carried by the name.
            if (data.size() % 12 != 0 || data.empty())
            {
                lucent::error("xenos-translate",
                              "{}: raw input size {} is not a non-zero multiple of 12", stem,
                              data.size());
                continue;
            }
            if (stem.rfind("vs_", 0) == 0)
            {
                c.type = xe::gpu::xenos::ShaderType::kVertex;
            }
            else if (stem.rfind("ps_", 0) == 0)
            {
                c.type = xe::gpu::xenos::ShaderType::kPixel;
            }
            else
            {
                lucent::error("xenos-translate",
                              "{}: raw input name does not start with vs_ or ps_", stem);
                continue;
            }
            c.ucode = reinterpret_cast<const uint32_t *>(data.data());
            c.ucode_dwords = data.size() / 4;
        }
        else if (!ParseContainer(data, &c, &error))
        {
            lucent::error("xenos-translate", "{}: container rejected: {}", stem, error);
            continue;
        }
        ++parsed;

        xe::gpu::SpirvShader shader(c.type, /*ucode_data_hash=*/uint64_t(i), c.ucode,
                                    c.ucode_dwords, std::endian::big);
        xe::StringBuffer disasm_buffer;
        shader.AnalyzeUcode(disasm_buffer);
        if (!shader.is_ucode_analyzed())
        {
            lucent::error("xenos-translate", "{}: microcode analysis failed", stem);
            continue;
        }
        ++analyzed;
        {
            std::ofstream d(outdir / (stem + ".ucode.txt"));
            d << shader.ucode_disassembly();
        }

        uint64_t modification = c.type == xe::gpu::xenos::ShaderType::kVertex
                                    ? translator.GetDefaultVertexShaderModification(
                                          shader.GetDynamicAddressableRegisterCount(0))
                                    : translator.GetDefaultPixelShaderModification(
                                          shader.GetDynamicAddressableRegisterCount(0));
        xe::gpu::Shader::Translation *translation = shader.GetOrCreateTranslation(modification);
        if (!translator.TranslateAnalyzedShader(*translation) || !translation->is_valid())
        {
            lucent::error("xenos-translate", "{}: translation failed", stem);
            continue;
        }
        const std::vector<uint8_t> &spirv = translation->translated_binary();
        if (spirv.empty())
        {
            lucent::error("xenos-translate", "{}: translation produced no output", stem);
            continue;
        }
        ++translated;
        std::ofstream o(outdir / (stem + ".spv"), std::ios::binary);
        o.write(reinterpret_cast<const char *>(spirv.data()), std::streamsize(spirv.size()));
        lucent::info("xenos-translate", "{}: {} microcode dwords, {} SPIR-V bytes", stem,
                     c.ucode_dwords, spirv.size());
    }

    lucent::info("xenos-translate", "{} inputs: {} parsed, {} analyzed, {} translated", total,
                 parsed, analyzed, translated);
    return translated == total ? 0 : 1;
}

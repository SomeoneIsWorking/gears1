// Writes the first mip of every Texture2D in a package whose object path
// contains a filter, as DDS files, through the native engine's texture layer.
// A maintainer tool for checking decoded textures by eye; the output is
// derived from the user's disc and belongs under scratch/.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/log.h>

#include "object/serialized_object.h"
#include "package/content_files.h"
#include "package/package.h"
#include "package/package_store.h"
#include "texture/texture2d.h"

namespace
{

namespace fs = std::filesystem;
using gears::engine::texture::PixelFormat;

void PutU32(std::vector<std::uint8_t> &out, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32U; shift += 8U)
    {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

// A minimal DDS container: compressed formats by FourCC, 32-bit colour and
// 8-bit luminance by channel masks.
std::vector<std::uint8_t> DdsFile(PixelFormat format, std::size_t width, std::size_t height,
                                  const std::vector<std::uint8_t> &data)
{
    std::vector<std::uint8_t> out{'D', 'D', 'S', ' '};
    PutU32(out, 124);
    PutU32(out, 0x1007U); // caps, height, width, pixel format
    PutU32(out, static_cast<std::uint32_t>(height));
    PutU32(out, static_cast<std::uint32_t>(width));
    PutU32(out, static_cast<std::uint32_t>(data.size()));
    PutU32(out, 0);
    PutU32(out, 1);
    for (int i = 0; i < 11; ++i)
    {
        PutU32(out, 0);
    }
    PutU32(out, 32);
    auto four_cc = [&](std::string_view code)
    {
        PutU32(out, 0x4U);
        out.insert(out.end(), code.begin(), code.end());
        for (int i = 0; i < 5; ++i)
        {
            PutU32(out, 0);
        }
    };
    switch (format)
    {
    case PixelFormat::Dxt1:
        four_cc("DXT1");
        break;
    case PixelFormat::Dxt3:
        four_cc("DXT3");
        break;
    case PixelFormat::Dxt5:
        four_cc("DXT5");
        break;
    case PixelFormat::A8R8G8B8:
        // Stored as B, G, R, A bytes once each word is in host order.
        PutU32(out, 0x41U);
        PutU32(out, 0);
        PutU32(out, 32);
        PutU32(out, 0x00FF0000U);
        PutU32(out, 0x0000FF00U);
        PutU32(out, 0x000000FFU);
        PutU32(out, 0xFF000000U);
        break;
    case PixelFormat::G8:
        PutU32(out, 0x20000U);
        PutU32(out, 0);
        PutU32(out, 8);
        PutU32(out, 0xFFU);
        PutU32(out, 0);
        PutU32(out, 0);
        PutU32(out, 0);
        break;
    default:
        throw std::runtime_error("no DDS mapping for this format");
    }
    PutU32(out, 0x1000U);
    for (int i = 0; i < 4; ++i)
    {
        PutU32(out, 0);
    }
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

std::string SafeFileName(std::string path)
{
    for (char &c : path)
    {
        if (c == '.' || c == '/' || c == '\\')
        {
            c = '_';
        }
    }
    return path;
}

int Run(const fs::path &package_path, std::string_view filter, const fs::path &out_dir)
{
    gears::engine::package::ContentFiles files(package_path.parent_path());
    gears::engine::package::PackageStore store(files);
    gears::engine::object::ClassHierarchy classes(store);
    const auto &package = store.Load(package_path.stem().string());
    fs::create_directories(out_dir);
    std::size_t written = 0;
    std::size_t candidates = 0;
    for (std::size_t i = 0; i < package.Tables().exports.size(); ++i)
    {
        auto index = static_cast<std::int32_t>(i + 1U);
        if (package.ClassName(index) != "Texture2D")
        {
            continue;
        }
        std::string path = package.ObjectPath(index);
        if (path.find(filter) == std::string::npos)
        {
            continue;
        }
        ++candidates;
        auto object = gears::engine::object::SerializedObject::Read(package, i, classes);
        auto texture = gears::engine::texture::Texture2D::Read(object);
        if (texture.Mips().empty())
        {
            lucent::error("texture-export", "{}: no mips", path);
            continue;
        }
        const auto &mip = texture.Mips()[0];
        fs::path out = out_dir / (SafeFileName(path) + ".dds");
        std::ofstream stream(out, std::ios::binary);
        std::vector<std::uint8_t> dds =
            DdsFile(texture.Format(), mip.width, mip.height, texture.LinearMip(0, files));
        stream.write(reinterpret_cast<const char *>(dds.data()),
                     static_cast<std::streamsize>(dds.size()));
        lucent::info("texture-export", "{} {} {}x{} ({} mips) -> {}", path,
                     gears::engine::texture::NameOf(texture.Format()), mip.width, mip.height,
                     texture.Mips().size(), out.filename().string());
        ++written;
    }
    lucent::info("texture-export", "{} of {} matching Texture2D export(s) written", written,
                 candidates);
    return written == 0U ? 1 : 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        lucent::error("texture-export",
                      "usage: gears_texture_export <package> <path filter> <output directory>");
        return 2;
    }
    return Run(argv[1], argv[2], argv[3]);
}

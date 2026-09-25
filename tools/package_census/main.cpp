// Loads every cooked package in a directory through the native engine's
// package layer and reports what it decoded. A package that fails to decode
// fails the census by name; an empty or missing directory is a refusal, not a
// clean result.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <lucent/log.h>

#include "object/object_resolver.h"
#include "object/serialized_object.h"
#include "package/content_files.h"
#include "package/lzo1x.h"
#include "bsp/bsp_model.h"
#include "bsp/component_geometry.h"
#include "material/material_surfaces.h"
#include "mesh/static_mesh.h"
#include "texture/texture2d.h"
#include "package/package_store.h"
#include "package/package.h"

namespace
{

namespace fs = std::filesystem;
using gears::engine::package::Package;

// Export serial data must tile the package after the header: sorted by
// offset, each range starts where the previous ended and the last ends at the
// package end. A layout misread shifts these ranges and breaks the tiling.
std::string CheckExportTiling(const Package &package)
{
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    for (const auto &object : package.Tables().exports)
    {
        if (object.serial_size != 0U)
        {
            ranges.emplace_back(object.serial_offset, object.serial_size);
        }
    }
    std::ranges::sort(ranges);
    std::size_t cursor = package.Summary().total_header_size;
    for (const auto &[offset, size] : ranges)
    {
        if (offset != cursor)
        {
            return "export data at " + std::to_string(offset) + " follows " +
                   std::to_string(cursor);
        }
        cursor += size;
    }
    if (cursor != package.Bytes().size())
    {
        return "export data ends at " + std::to_string(cursor) + " of " +
               std::to_string(package.Bytes().size());
    }
    return {};
}

struct Census
{
    std::size_t packages = 0;
    std::size_t failed = 0;
    std::size_t names = 0;
    std::size_t imports = 0;
    std::size_t exports = 0;
    std::size_t uncompressed_bytes = 0;
    std::map<std::string, std::size_t> export_classes;
    std::size_t objects = 0;
    std::size_t properties = 0;
    // Per class: property streams that failed, and streams that end exactly
    // at the export's end (the class adds no native data).
    std::map<std::string, std::size_t> property_failures;
    std::map<std::string, std::size_t> property_only;
    std::map<std::string, std::size_t> texture_formats;
    std::map<std::size_t, std::size_t> mesh_lod_counts;
    // Materials by what gives them their base colour.
    std::map<std::string, std::size_t> material_diffuse;
    std::size_t bsp_models = 0;
    std::size_t bsp_components = 0;
    std::size_t bsp_triangles = 0;
};

// Decodes the native data of the asset classes the engine owns so far. Its
// caches are keyed by package, so it lives no longer than the package it
// decodes.
class AssetDecoder
{
  public:
    AssetDecoder(gears::engine::object::ClassHierarchy &classes,
                 gears::engine::object::ObjectResolver &resolver)
        : materials_(classes, resolver), components_(classes, resolver)
    {
    }

    void Decode(const gears::engine::object::SerializedObject &object,
                const std::string &class_name, Census &census)
    {
        if (class_name == "Material" || class_name == "MaterialInstanceConstant")
        {
            gears::engine::object::ExportLocation location{
                &object.Owner(), static_cast<std::size_t>(object.Index()) - 1U};
            ++census.material_diffuse[std::string(
                gears::engine::material::NameOf(materials_.Surface(location).color.outcome))];
        }
        else if (class_name == "StaticMesh")
        {
            auto mesh = gears::engine::mesh::StaticMesh::Read(object);
            ++census.mesh_lod_counts[mesh.Lods().size()];
        }
        else if (class_name == "Texture2D")
        {
            auto texture = gears::engine::texture::Texture2D::Read(object);
            ++census.texture_formats[std::string(gears::engine::texture::NameOf(texture.Format()))];
        }
        else if (class_name == "Model")
        {
            (void)gears::engine::bsp::BspModel::Read(object);
            ++census.bsp_models;
        }
        else if (class_name == "ModelComponent")
        {
            auto geometry = components_.Triangulate(object);
            ++census.bsp_components;
            census.bsp_triangles += geometry.indices.size() / 3U;
        }
    }

  private:
    gears::engine::material::MaterialSurfaces materials_;
    gears::engine::bsp::ComponentGeometry components_;
};

// Reads every non-schema export's property stream. A failure is counted
// against its class, never skipped.
void ReadProperties(const Package &package, const std::string &file,
                    gears::engine::object::ClassHierarchy &classes,
                    gears::engine::object::ObjectResolver &resolver, Census &census)
{
    AssetDecoder decoder(classes, resolver);
    for (std::size_t i = 0; i < package.Tables().exports.size(); ++i)
    {
        if (gears::engine::object::IsSchemaExport(package, i))
        {
            continue;
        }
        std::string class_name = package.ClassName(static_cast<std::int32_t>(i + 1U));
        ++census.objects;
        try
        {
            auto object = gears::engine::object::SerializedObject::Read(package, i, classes);
            census.properties += object.Properties().size();
            if (object.NativeData().empty())
            {
                ++census.property_only[class_name];
            }
            if (!object.IsClassDefault())
            {
                decoder.Decode(object, class_name, census);
            }
        }
        catch (const gears::engine::package::PackageFormatError &error)
        {
            if (census.property_failures[class_name]++ == 0U)
            {
                lucent::error("package-census", "{} export {} ({}): {}", file, i + 1U, class_name,
                              error.what());
            }
        }
    }
}

void LoadOne(const fs::path &path, gears::engine::object::ClassHierarchy &classes,
             gears::engine::object::ObjectResolver &resolver, Census &census)
{
    ++census.packages;
    try
    {
        Package package =
            Package::Load(path.stem().string(), gears::engine::package::ReadPackageFile(path));
        std::string tiling = CheckExportTiling(package);
        if (!tiling.empty())
        {
            ++census.failed;
            lucent::error("package-census", "{}: {}", path.filename().string(), tiling);
            return;
        }
        census.names += package.Tables().names.size();
        census.imports += package.Tables().imports.size();
        census.exports += package.Tables().exports.size();
        census.uncompressed_bytes += package.Bytes().size();
        for (std::size_t i = 0; i < package.Tables().exports.size(); ++i)
        {
            ++census.export_classes[package.ClassName(static_cast<std::int32_t>(i + 1U))];
        }
        ReadProperties(package, path.filename().string(), classes, resolver, census);
    }
    // The census boundary: a format refusal is this package's result, and the
    // next file is independent of it.
    catch (const gears::engine::package::PackageFormatError &error)
    {
        ++census.failed;
        lucent::error("package-census", "{}: {}", path.filename().string(), error.what());
    }
    catch (const gears::engine::package::DecompressionError &error)
    {
        ++census.failed;
        lucent::error("package-census", "{}: {}", path.filename().string(), error.what());
    }
}

int Run(const fs::path &directory)
{
    if (!fs::is_directory(directory))
    {
        lucent::error("package-census", "REFUSING: {} is not a directory, so nothing was loaded",
                      directory.string());
        return 2;
    }
    std::vector<fs::path> files;
    for (const fs::directory_entry &entry : fs::directory_iterator(directory))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".xxx")
        {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    if (files.empty())
    {
        lucent::error("package-census", "REFUSING: {} holds no .xxx packages", directory.string());
        return 2;
    }
    // Script packages the class hierarchy loads stay cached; the census's
    // own packages are loaded one at a time.
    gears::engine::package::ContentFiles content(directory);
    gears::engine::package::PackageStore scripts(content);
    gears::engine::object::ClassHierarchy hierarchy(scripts);
    gears::engine::object::ObjectResolver resolver(scripts);
    Census census;
    for (const fs::path &path : files)
    {
        LoadOne(path, hierarchy, resolver, census);
    }
    lucent::info("package-census", "{} intrinsic class(es) without a script export",
                 hierarchy.IntrinsicClasses().size());
    std::vector<std::pair<std::size_t, std::string>> classes;
    classes.reserve(census.export_classes.size());
    for (const auto &[name, count] : census.export_classes)
    {
        classes.emplace_back(count, name);
    }
    std::ranges::sort(classes, std::greater<>());
    for (std::size_t i = 0; i < std::min<std::size_t>(classes.size(), 25U); ++i)
    {
        lucent::info("package-census", "  {:>8} {}", classes[i].first, classes[i].second);
    }
    lucent::info("package-census",
                 "{} of {} package(s) decoded; {} failed; {} names, {} imports, {} exports, "
                 "{} export classes, {} MiB uncompressed",
                 census.packages - census.failed, census.packages, census.failed, census.names,
                 census.imports, census.exports, census.export_classes.size(),
                 census.uncompressed_bytes >> 20U);
    std::size_t property_failures = 0;
    for (const auto &[name, count] : census.property_failures)
    {
        property_failures += count;
        lucent::error("package-census", "  property streams failed: {:>8} {}", count, name);
    }
    for (const auto &[lods, count] : census.mesh_lod_counts)
    {
        lucent::info("package-census", "  StaticMesh {:>8} with {} LOD(s)", count, lods);
    }
    for (const auto &[format, count] : census.texture_formats)
    {
        lucent::info("package-census", "  Texture2D {:>8} {}", count, format);
    }
    lucent::info("package-census", "  BSP: {} model(s); {} component(s) of {} triangle(s)",
                 census.bsp_models, census.bsp_components, census.bsp_triangles);
    for (const auto &[outcome, count] : census.material_diffuse)
    {
        lucent::info("package-census", "  material base colour {:>8} {}", count, outcome);
    }
    lucent::info("package-census",
                 "{} of {} object property stream(s) read ({} properties); {} failed; {} "
                 "classes had streams that end their export",
                 census.objects - property_failures, census.objects, census.properties,
                 property_failures, census.property_only.size());
    return census.failed == 0U && property_failures == 0U ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        lucent::error("package-census", "usage: gears_package_census <CookedXenon directory>");
        return 2;
    }
    return Run(argv[1]);
}

// Renders a level package and every sublevel it streams, headlessly through the
// native engine's Vulkan renderer, and writes the frame as a binary PPM. A
// maintainer tool: the image is derived from the user's disc and belongs
// under scratch/.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <lucent/log.h>

#include "object/class_hierarchy.h"
#include "object/object_resolver.h"
#include "package/content_files.h"
#include "package/package_store.h"
#include "scene/camera.h"
#include "render/level_renderer.h"
#include "render/vulkan_device.h"
#include "scene/level_scene.h"
#include "scene/static_meshes.h"
#include "scene/world.h"

namespace
{

namespace fs = std::filesystem;
using gears::engine::mesh::Vector3;

constexpr VkExtent2D kImageExtent{1280, 720};
// Placements outside these quantiles (skyboxes, distant vistas) do not
// stretch the overview camera's framing.
constexpr double kFramingLowQuantile = 0.1;
constexpr double kFramingHighQuantile = 0.9;

float Quantile(std::vector<float> values, double q)
{
    auto rank = static_cast<std::size_t>(q * static_cast<double>(values.size() - 1U));
    std::ranges::nth_element(values, values.begin() + static_cast<std::ptrdiff_t>(rank));
    return values[rank];
}

// A three-quarter overview of where the level's meshes and BSP vertices
// stand.
gears::engine::scene::Camera OverviewCamera(const gears::engine::scene::World &world)
{
    std::array<std::vector<float>, 3> axes;
    for (const auto &level : world.Levels())
    {
        const auto &scene = level.scene;
        for (const auto &instance : scene.Instances())
        {
            for (std::size_t axis = 0; axis < 3U; ++axis)
            {
                axes[axis].push_back(instance.world.m[3][axis]);
            }
        }
        for (const auto &model : scene.Models())
        {
            for (const auto &vertex : model.geometry.lod.vertices)
            {
                axes[0].push_back(vertex.position.x);
                axes[1].push_back(vertex.position.y);
                axes[2].push_back(vertex.position.z);
            }
        }
    }
    std::array<float, 3> low{};
    std::array<float, 3> high{};
    for (std::size_t axis = 0; axis < 3U; ++axis)
    {
        low[axis] = Quantile(axes[axis], kFramingLowQuantile);
        high[axis] = Quantile(axes[axis], kFramingHighQuantile);
    }
    Vector3 center{(low[0] + high[0]) * 0.5F, (low[1] + high[1]) * 0.5F, (low[2] + high[2]) * 0.5F};
    float span = std::max({high[0] - low[0], high[1] - low[1], 1000.0F});
    gears::engine::scene::Camera camera;
    camera.target = center;
    camera.eye = {center.x - span * 0.6F, center.y - span * 0.6F, center.z + span * 0.5F};
    camera.aspect =
        static_cast<float>(kImageExtent.width) / static_cast<float>(kImageExtent.height);
    return camera;
}

void WritePpm(const fs::path &path, VkExtent2D extent, const std::vector<std::uint8_t> &rgba)
{
    std::ofstream stream(path, std::ios::binary);
    stream << "P6\n" << extent.width << ' ' << extent.height << "\n255\n";
    for (std::size_t pixel = 0; pixel < rgba.size(); pixel += 4U)
    {
        stream.write(reinterpret_cast<const char *>(&rgba[pixel]), 3);
    }
    if (!stream)
    {
        throw std::runtime_error("cannot write " + path.string());
    }
}

int Run(const fs::path &level_path, const fs::path &out_path)
{
    gears::engine::package::ContentFiles files(level_path.parent_path());
    gears::engine::package::PackageStore store(files);
    gears::engine::object::ClassHierarchy classes(store);
    gears::engine::object::ObjectResolver resolver(store);
    auto world =
        gears::engine::scene::World::Load(store, classes, resolver, level_path.stem().string());
    std::size_t placed = 0;
    std::size_t models = 0;
    for (const auto &level : world.Levels())
    {
        const auto &census = level.scene.Census();
        placed += census.placed;
        models += census.model_components;
        lucent::info("level-render", "{}: {} placed static mesh(es), {} model component(s)",
                     level.package->Name(), census.placed, census.model_components);
    }
    if (placed == 0U && models == 0U)
    {
        lucent::error("level-render", "REFUSING: {} places no static meshes or BSP",
                      world.Persistent().Name());
        return 1;
    }

    gears::engine::scene::StaticMeshes static_meshes(classes);
    gears::engine::render::VulkanDevice device;
    gears::engine::render::LevelRenderer renderer(device, kImageExtent, files, classes, resolver,
                                                  static_meshes);
    for (const auto &level : world.Levels())
    {
        renderer.Prepare(*level.package, level.scene);
    }
    auto camera = OverviewCamera(world);
    WritePpm(out_path, renderer.Extent(), renderer.Render(camera));
    const auto &drawn = renderer.Census();
    const auto &materials = renderer.Materials();
    for (const auto &[source, count] : materials.colors)
    {
        lucent::info("level-render", "  section colour   {:>6} {}", count, source);
    }
    for (const auto &[blend, count] : materials.blends)
    {
        lucent::info("level-render", "  section blend    {:>6} {}", count, blend);
    }
    for (const auto &[lighting, count] : materials.lighting)
    {
        lucent::info("level-render", "  section lighting {:>6} {}", count, lighting);
    }
    lucent::info("level-render",
                 "{} level(s): {} section draw(s), {} mesh(es), {} BSP component(s), {} "
                 "texture(s) on {} -> {}",
                 world.Levels().size(), drawn.draws, drawn.meshes, drawn.models, materials.textures,
                 device.Name(), out_path.filename().string());
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        lucent::error("level-render", "usage: gears_level_render <level package> <output.ppm>");
        return 2;
    }
    return Run(argv[1], argv[2]);
}

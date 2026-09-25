#include "mesh_renderer.h"

#include <array>
#include <cstring>
#include <span>

#include "package/byte_reader.h"
#include "spirv/mesh_frag.h"
#include "spirv/mesh_vert.h"

namespace gears::engine::render
{
namespace
{

struct PushConstants
{
    scene::Matrix world;
    scene::Matrix view_projection;
};

VkShaderModule CreateShader(VkDevice device, std::span<const std::uint32_t> words)
{
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = words.size_bytes();
    info.pCode = words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    Check(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
}

// Shader modules are needed only while the pipeline is created.
class ShaderModules
{
  public:
    explicit ShaderModules(VkDevice device)
        : device_(device), vertex_(CreateShader(device, spirv::kMesh_vert)),
          fragment_(CreateShader(device, spirv::kMesh_frag))
    {
    }
    ~ShaderModules()
    {
        vkDestroyShaderModule(device_, vertex_, nullptr);
        vkDestroyShaderModule(device_, fragment_, nullptr);
    }
    ShaderModules(const ShaderModules &) = delete;
    ShaderModules &operator=(const ShaderModules &) = delete;

    [[nodiscard]] std::array<VkPipelineShaderStageCreateInfo, 2> Stages() const
    {
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex_;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment_;
        stages[1].pName = "main";
        return stages;
    }

  private:
    VkDevice device_;
    VkShaderModule vertex_;
    VkShaderModule fragment_;
};

void PackVertex(const mesh::MeshVertex &vertex, std::span<std::uint8_t> out)
{
    std::array<float, 8> packed{vertex.position.x, vertex.position.y, vertex.position.z,
                                vertex.normal.x,   vertex.normal.y,   vertex.normal.z,
                                vertex.uv[0][0],   vertex.uv[0][1]};
    std::memcpy(out.data(), packed.data(), sizeof(packed));
}

std::vector<GpuSection> CheckedSections(const mesh::StaticMeshLod &lod)
{
    if (lod.indices.empty() || lod.vertices.empty())
    {
        throw package::PackageFormatError("static mesh LOD has no triangles to upload");
    }
    // StaticMesh::Read has placed every section inside the index list.
    std::vector<GpuSection> sections;
    sections.reserve(lod.sections.size());
    for (const mesh::MeshSection &section : lod.sections)
    {
        sections.push_back({section.first_index, section.triangle_count * 3U});
    }
    return sections;
}

} // namespace

GpuMesh::GpuMesh(const VulkanDevice &device, const mesh::StaticMeshLod &lod)
    : vertices_(device.CreateHostBuffer(VkDeviceSize{kVertexStride} * lod.vertices.size(),
                                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)),
      indices_(device.CreateHostBuffer(sizeof(std::uint16_t) * lod.indices.size(),
                                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT)),
      sections_(CheckedSections(lod))
{
    std::span<std::uint8_t> vertex_bytes = vertices_.Bytes();
    for (std::size_t i = 0; i < lod.vertices.size(); ++i)
    {
        PackVertex(lod.vertices[i], vertex_bytes.subspan(i * kVertexStride, kVertexStride));
    }
    std::memcpy(indices_.Bytes().data(), lod.indices.data(),
                sizeof(std::uint16_t) * lod.indices.size());
}

MeshRenderer::MeshRenderer(const VulkanDevice &device, const OffscreenTarget &target,
                           VkDescriptorSetLayout texture_layout)
    : device_(device.Device()), extent_(target.Extent())
{
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &texture_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    Check(vkCreatePipelineLayout(device_, &layout, nullptr, &layout_), "vkCreatePipelineLayout");

    ShaderModules shaders(device_);
    std::array<VkPipelineShaderStageCreateInfo, 2> stages = shaders.Stages();

    VkVertexInputBindingDescription binding{0, GpuMesh::kVertexStride, VK_VERTEX_INPUT_RATE_VERTEX};
    std::array<VkVertexInputAttributeDescription, 3> attributes{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},
    }};
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    // Both faces are drawn until the cooked winding convention is measured.
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                 VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();

    VkGraphicsPipelineCreateInfo pipeline{};
    pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.stageCount = static_cast<std::uint32_t>(stages.size());
    pipeline.pStages = stages.data();
    pipeline.pVertexInputState = &vertex_input;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pDepthStencilState = &depth;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = layout_;
    pipeline.renderPass = target.RenderPass();
    Check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_),
          "vkCreateGraphicsPipelines");
}

MeshRenderer::~MeshRenderer()
{
    vkDestroyPipeline(device_, pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, layout_, nullptr);
}

void MeshRenderer::Bind(VkCommandBuffer commands) const
{
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    VkViewport viewport{
        0.0F, 0.0F, static_cast<float>(extent_.width), static_cast<float>(extent_.height),
        0.0F, 1.0F};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetScissor(commands, 0, 1, &scissor);
}

void MeshRenderer::Draw(VkCommandBuffer commands, const GpuMesh &mesh, const GpuSection &section,
                        VkDescriptorSet texture, const scene::Matrix &world,
                        const scene::Matrix &view_projection) const
{
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &texture, 0,
                            nullptr);
    PushConstants constants{world, view_projection};
    vkCmdPushConstants(commands, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(constants),
                       &constants);
    VkBuffer vertices = mesh.Vertices();
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commands, 0, 1, &vertices, &offset);
    vkCmdBindIndexBuffer(commands, mesh.Indices(), 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(commands, section.index_count, 1, section.first_index, 0, 0);
}

} // namespace gears::engine::render

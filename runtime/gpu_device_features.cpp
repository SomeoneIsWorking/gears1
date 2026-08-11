#include "gpu_device_features.h"

namespace gears
{

void SelectDeviceFeatures(VkPhysicalDevice physical,
                          VkPhysicalDeviceFeatures& enable,
                          DeviceCapabilities& caps)
{
    VkPhysicalDeviceFeatures available{};
    vkGetPhysicalDeviceFeatures(physical, &available);

    enable = VkPhysicalDeviceFeatures{};

    // The translated shaders read a storage buffer from the vertex stage; the pixel
    // stage samples. Reads never need the stores/atomics features, but the
    // translator declares the buffer writable, so the declaration has to be
    // honoured even though nothing writes through it.
    enable.vertexPipelineStoresAndAtomics = available.vertexPipelineStoresAndAtomics;
    enable.fragmentStoresAndAtomics = available.fragmentStoresAndAtomics;

    enable.pipelineStatisticsQuery = available.pipelineStatisticsQuery;
    caps.pipelineStatistics = available.pipelineStatisticsQuery != VK_FALSE;

    enable.geometryShader = available.geometryShader;
    caps.geometryShader = available.geometryShader != VK_FALSE;

    enable.depthClamp = available.depthClamp;
    caps.depthClamp = available.depthClamp != VK_FALSE;

    enable.shaderStorageImageReadWithoutFormat =
        available.shaderStorageImageReadWithoutFormat;
    enable.shaderStorageImageWriteWithoutFormat =
        available.shaderStorageImageWriteWithoutFormat;
    caps.storageImageWithoutFormat =
        available.shaderStorageImageReadWithoutFormat != VK_FALSE &&
        available.shaderStorageImageWriteWithoutFormat != VK_FALSE;
}

} // namespace gears

#include "gpu_instance.h"

#include <algorithm>

namespace gears
{

bool InstanceExtensionsSatisfy(const std::vector<std::string>& created,
                               const std::vector<std::string>& required,
                               std::vector<std::string>& missing)
{
    missing.clear();
    for (const std::string& want : required)
    {
        if (std::find(created.begin(), created.end(), want) == created.end())
            missing.push_back(want);
    }
    return missing.empty();
}

std::vector<std::string> MergeInstanceExtensions(
    const std::vector<std::string>& first,
    const std::vector<std::string>& second)
{
    std::vector<std::string> merged = first;
    for (const std::string& name : second)
    {
        if (std::find(merged.begin(), merged.end(), name) == merged.end())
            merged.push_back(name);
    }
    return merged;
}

} // namespace gears

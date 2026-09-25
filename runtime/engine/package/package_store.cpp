#include "package_store.h"

#include <algorithm>
#include <cctype>

namespace gears::engine::package
{

const Package &PackageStore::Load(std::string_view name)
{
    std::string key(name);
    std::ranges::transform(key, key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto found = packages_.find(key);
    if (found != packages_.end())
    {
        return *found->second;
    }
    auto package = std::make_unique<Package>(Package::Load(std::string(name), files_.File(name)));
    return *packages_.emplace(key, std::move(package)).first->second;
}

} // namespace gears::engine::package

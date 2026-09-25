#pragma once

#include <string>
#include <vector>

#include "object/class_hierarchy.h"
#include "package/package.h"

namespace gears::engine::scene
{

// The sublevel packages a persistent level streams, by package name in its
// WorldInfo's order; empty for a level that streams none. Refuses a level
// without exactly one WorldInfo, and a streaming entry that is not one of
// the level's exports or names no package.
std::vector<std::string> StreamingLevelPackages(const package::Package &level,
                                                object::ClassHierarchy &classes);

} // namespace gears::engine::scene

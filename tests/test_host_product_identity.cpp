#include "host_product_identity.h"

#include <string_view>

static_assert(std::string_view(gears::kHostProductName) == "GearsUE3");
static_assert(std::string_view(gears::kHostProductKey) == "gearsue3");
static_assert(std::string_view(gears::kHostDrawApplicationName) == "gearsue3-draw");

int main()
{
}

// The only code of ours inside the Xenia build island.
//
// `xenia/base` declares a couple of host-integration entry points whose Linux
// implementations live in xenia/base/system_gnulinux.cc, which pulls in SDL2
// purely to put a message box on screen. The shader translator never needs a
// message box: the paths that would raise one are cvar-parse failures and
// fatal-log handling, neither of which can occur in a batch translation run.
//
// Rather than dragging SDL2 in for that, we define them here. They are
// deliberately loud and non-silent: if one is ever reached, the project logger
// records it instead of pretending nothing happened.

#include <string>
#include <string_view>

#include "lucent/log.h"
#include "xenia/base/system.h"

namespace xe
{
namespace
{
const char *MessageBoxKind(SimpleMessageBoxType type)
{
    switch (type)
    {
    case SimpleMessageBoxType::Help:
        return "help";
    case SimpleMessageBoxType::Warning:
        return "warning";
    case SimpleMessageBoxType::Error:
        return "error";
    }
    return "unknown";
}
} // namespace

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message)
{
    lucent::error("xenia", "{}: {}", MessageBoxKind(type), message);
}

void LaunchWebBrowser(const std::string_view url)
{
    lucent::error("xenia", "refusing to open a browser for {}", url);
}

void LaunchFileExplorer(const std::filesystem::path &url)
{
    lucent::error("xenia", "refusing to open a file explorer for {}", url.string());
}

} // namespace xe

// The only code of ours inside the Xenia build island.
//
// `xenia/base` declares a couple of host-integration entry points whose Linux
// implementations live in xenia/base/system_gnulinux.cc, which pulls in SDL2
// purely to put a message box on screen. The shader translator never needs a
// message box: the paths that would raise one are cvar-parse failures and
// fatal-log handling, neither of which can occur in a batch translation run.
//
// Rather than dragging SDL2 in for that, we define them here. They are
// deliberately loud and non-silent: if one is ever reached, the run says so on
// stderr instead of pretending nothing happened.

#include <cstdio>
#include <string>
#include <string_view>

#include "xenia/base/system.h"

namespace xe {

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  const char* kind = "message";
  switch (type) {
    case SimpleMessageBoxType::Help:
      kind = "help";
      break;
    case SimpleMessageBoxType::Warning:
      kind = "warning";
      break;
    case SimpleMessageBoxType::Error:
      kind = "error";
      break;
  }
  std::fprintf(stderr, "[xenia %s] %.*s\n", kind, int(message.size()),
               message.data());
}

void LaunchWebBrowser(const std::string_view url) {
  std::fprintf(stderr, "[xenia] refusing to open a browser for %.*s\n",
               int(url.size()), url.data());
}

void LaunchFileExplorer(const std::filesystem::path& url) {
  std::fprintf(stderr, "[xenia] refusing to open a file explorer for %s\n",
               url.string().c_str());
}

}  // namespace xe

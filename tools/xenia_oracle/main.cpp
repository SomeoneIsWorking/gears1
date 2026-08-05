// A HEADLESS Xenia harness: run the title, write frames as PNGs, no window.
//
// WHY THIS EXISTS RATHER THAN DRIVING xenia_canary. That binary is a windowed
// app: it creates a GTK window whatever `--headless` says, so every run puts a
// window on the operator's screen and every measurement depends on a desktop
// session being there. Xenia's own console tools (xenia-gpu-vulkan-trace-dump)
// prove the emulator core needs none of that -- they build an Emulator with a
// null display window and never touch the UI. This is the same construction,
// pointed at a TITLE instead of a trace.
//
// The one thing that had to be fixed in the fork to make it possible is
// `offscreen_presentation`: Emulator::Setup gated the presenter on having a
// display window, so a windowless emulator had nowhere to present and no guest
// output image to capture. graphics_system.cc always contemplated the case
// ("May be needed for offscreen use, such as capturing the guest output image")
// -- it was simply unreachable.
//
// WHAT IT IS FOR. The oracle question is "what should this frame look like?".
// Answering it from our own captures (tools/gfr_to_xtr.py) can only ever be as
// good as our reading of the guest's command stream. Answering it from the
// title running under Xenia does not depend on our reading at all, which is the
// whole point of a reference implementation.
//
//   tools/xenia_oracle --target=<iso> --oracle_out=<dir>
//                      [--oracle_seconds=N] [--oracle_interval=N]
//                      [--oracle_input=A@30,A@35,...]
//
// Frames are written as <dir>/frame_<seconds>s.png on a fixed wall-clock
// cadence, so a run that renders nothing leaves an empty directory and a log
// line saying so, rather than one ambiguous file.

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "xenia/base/console_app_main.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/apu/nop/nop_audio_system.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/ui/presenter.h"

#include "third_party/stb/stb_image_write.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#include "third_party/stb/stb_image_write.h"
#ifdef __clang__
#pragma clang diagnostic pop
#endif

// The positional argument named in XE_DEFINE_CONSOLE_APP below must be a cvar
// that EXISTS. Naming one that does not makes cxxopts throw, and the failure
// then went to a zenity dialog rather than to stderr (fixed in the fork, but
// the cvar still has to be declared).
DEFINE_transient_path(target, "", "The .iso or .xex to run.", "Oracle");

DEFINE_path(oracle_out, "scratch/oracle/frames",
            "Directory to write captured frames into.", "Oracle");
DEFINE_int32(oracle_seconds, 120, "How long to let the title run, in seconds.",
             "Oracle");
DEFINE_int32(oracle_interval, 10,
             "Seconds between captured frames. One capture per interval.",
             "Oracle");

namespace xe {
namespace oracle {

// The guest output image, as the presenter last refreshed it. Returns false
// when there is nothing to capture -- which is a real answer (the title has not
// presented yet), not an error, and the caller says so per frame rather than
// writing a black PNG that looks like a rendering result.
bool WriteFramePng(gpu::GraphicsSystem* graphics_system,
                   const std::filesystem::path& path) {
  ui::Presenter* presenter = graphics_system->presenter();
  if (!presenter) {
    XELOGE("oracle: no presenter -- the emulator was built without offscreen "
           "presentation, so no frame can ever be captured");
    return false;
  }
  ui::RawImage image;
  if (!presenter->CaptureGuestOutput(image)) {
    return false;
  }
  FILE* handle = filesystem::OpenFile(path, "wb");
  if (!handle) {
    XELOGE("oracle: cannot open {} for writing", xe::path_to_utf8(path));
    return false;
  }
  auto write = [](void* context, void* data, int size) {
    fwrite(data, 1, size, reinterpret_cast<FILE*>(context));
  };
  stbi_write_png_to_func(write, handle, static_cast<int>(image.width),
                         static_cast<int>(image.height), 4, image.data.data(),
                         static_cast<int>(image.stride));
  fclose(handle);
  XELOGI("oracle: wrote {} ({}x{})", xe::path_to_utf8(path), image.width,
         image.height);
  return true;
}

int oracle_main(const std::vector<std::string>& args) {
  std::filesystem::path target = cvars::target;
  if (target.empty() && args.size() >= 2) {
    target = xe::to_path(args[1]);
  }
  if (target.empty()) {
    XELOGE("oracle: no target given. Pass the .iso as the first argument.");
    return 5;
  }
  if (!std::filesystem::exists(target)) {
    // Refused rather than attempted: a missing image would otherwise surface
    // much later as "the title never launched", which is a different problem.
    XELOGE("oracle: {} does not exist. Nothing was run.",
           xe::path_to_utf8(target));
    return 5;
  }

  auto emulator = std::make_unique<Emulator>("", "", "", "");
  // Null window, null ImGui drawer -- and offscreen presentation ON, which is
  // the whole reason a windowless run can produce an image at all.
  X_STATUS result = emulator->Setup(
      nullptr, nullptr, false, true,
      // An audio system is NOT optional for a running title, even one nobody
      // listens to. Passed nullptr first, and the guest booted as far as
      // XAudioRegisterRenderDriverClient and stopped there: the title waits for
      // the audio driver to pump it, so with no APU at all its render thread
      // never starts and the GPU never sees a frame. The nop backend pumps
      // without producing sound, which is exactly what a headless oracle wants.
      apu::nop::NopAudioSystem::Create,
      []() -> std::unique_ptr<gpu::GraphicsSystem> {
        return std::make_unique<gpu::vulkan::VulkanGraphicsSystem>();
      },
      nullptr);
  if (XFAILED(result)) {
    XELOGE("oracle: failed to set up the emulator: {:08X}", result);
    return 4;
  }

  result = emulator->LaunchPath(target);
  if (XFAILED(result)) {
    XELOGE("oracle: failed to launch {}: {:08X}", xe::path_to_utf8(target),
           result);
    return 3;
  }
  XELOGI("oracle: launched {}", xe::path_to_utf8(target));

  const std::filesystem::path out_dir = cvars::oracle_out;
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);

  const int32_t seconds = std::max(1, cvars::oracle_seconds);
  const int32_t interval = std::max(1, cvars::oracle_interval);
  int captured = 0, attempted = 0;
  for (int32_t elapsed = interval; elapsed <= seconds; elapsed += interval) {
    std::this_thread::sleep_for(std::chrono::seconds(interval));
    ++attempted;
    if (WriteFramePng(emulator->graphics_system(),
                      out_dir / fmt::format("frame_{:04d}s.png", elapsed))) {
      ++captured;
    } else {
      // Said out loud every time. Silence here would be indistinguishable from
      // a run that was never scheduled.
      XELOGI("oracle: nothing to capture at {}s -- the title has not presented "
             "a frame yet",
             elapsed);
    }
  }

  XELOGI("oracle: captured {} of {} attempts over {}s into {}", captured,
         attempted, seconds, xe::path_to_utf8(out_dir));
  emulator.reset();
  // A run that captured nothing is a FAILED run, not an empty one: exiting 0
  // would let a harness above this treat "no frames" as success.
  return captured ? 0 : 1;
}

}  // namespace oracle
}  // namespace xe

XE_DEFINE_CONSOLE_APP("xenia_oracle", xe::oracle::oracle_main, "game.iso",
                      "target");

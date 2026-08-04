#!/bin/sh
# Prove the present path does not alter the frame -- with no window.
#
# Everything else this project captures comes from the renderer's own readback,
# which is BEFORE the blit into the swapchain. That left the one step deciding
# what a person actually sees outside every measurement here, and it could only be
# checked by opening a window on someone's desktop.
#
# VK_EXT_headless_surface removes that: a real surface backed by nothing, a real
# swapchain, the real blit, a real present. The title renders exactly ONE frame,
# so the presenter has a single image and repeats it, and the frame captured after
# the swapchain must then equal the renderer's own readback of it, pixel for pixel.
#
# Usage: tools/verify_present_path.sh [frame]
set -eu

frame="${1:-400}"
out="${GEARS_VERIFY_DIR:-scratch/verify/present}"
game="${GEARS_GAME_DIR:-scratch/game}"
binary="${GEARS_BUILD_DIR:-scratch/build}/runtime/gears1"

[ -f "$game/default.xex" ] || { echo "no game data at '$game'" >&2; exit 1; }
[ -x "$binary" ] || { echo "not built: $binary" >&2; exit 1; }

rm -rf "$out"; mkdir -p "$out"
GEARS_PRESENT_HEADLESS=1 GEARS_PRESENT_DUMP=1 \
GEARS_PRESENT_DUMP_AT=$((frame + 60)) GEARS_PRESENT_DUMP_DIR="$out" \
GEARS_DRAW_DIR="$out" GEARS_DRAW_FRAME_AT="$frame" GEARS_DRAW_FRAME_COUNT=1 \
GEARS_INPUT_SCRIPT="25000:START,25300:" \
    "$binary" "$game/default.xex" "$game" > "$out/run.log" 2>&1 &
pid=$!
trap 'kill -9 "$pid" 2>/dev/null || true' EXIT INT TERM

waited=0
while [ "$waited" -lt 300 ]; do
    [ -f "$out/presented_1.ppm" ] && [ -f "$out/frame.ppm" ] && break
    sleep 5; waited=$((waited + 5))
    kill -0 "$pid" 2>/dev/null || break
done
sleep 2
kill -9 "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

# REFUSE rather than pass when there is nothing to compare: a missing capture is
# the one outcome that would otherwise look like agreement.
if [ ! -f "$out/presented_1.ppm" ] || [ ! -f "$out/frame.ppm" ]; then
    echo "verify_present_path: NO CAPTURE. Wanted $out/presented_1.ppm and" >&2
    echo "  $out/frame.ppm; the run reached neither, so this says NOTHING about" >&2
    echo "  the present path. See $out/run.log" >&2
    exit 3
fi
grep -m1 "swapchain format" "$out/run.log" >&2 || true
python3 - "$out/presented_1.ppm" "$out/frame.ppm" <<'PY'
import sys
def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    parts = data.split(b'\n', 3)
    assert parts[0] == b'P6', path
    w, h = parts[1].split()
    return int(w), int(h), parts[3]
pw, ph, presented = read_ppm(sys.argv[1])
rw, rh, rendered = read_ppm(sys.argv[2])
if (pw, ph) != (rw, rh):
    print(f"FAIL: presented {pw}x{ph} but rendered {rw}x{rh}")
    sys.exit(1)
if presented == rendered:
    print(f"PASS: the presented frame is identical to the rendered one "
          f"({pw}x{ph}, {len(presented)} bytes) -- the swapchain blit alters nothing")
    sys.exit(0)
worst = max(abs(a - b) for a, b in zip(presented, rendered))
differing = sum(1 for a, b in zip(presented, rendered) if a != b)
print(f"FAIL: {differing} of {len(presented)} bytes differ, worst by {worst}. "
      f"The present path is changing the frame -- compare {sys.argv[1]} with {sys.argv[2]}")
sys.exit(1)
PY

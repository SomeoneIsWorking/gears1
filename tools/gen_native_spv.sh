#!/usr/bin/env bash
# Compile a native-pass GLSL shader to SPIR-V and emit it as a C++ header.
#
# The modules are checked in as headers rather than compiled at build time so the
# runtime has no shader-compiler dependency and a native pass cannot silently
# change under a driver update. That means the header is GENERATED and must be
# regenerated whenever the .frag changes -- which is what this script is for, so
# that step is one command rather than a remembered incantation.
#
#   tools/gen_native_spv.sh runtime/shaders/movie_yuv.frag \
#                           runtime/native_pass_movie_spv.h MovieYuvSpirv
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 <shader.frag> <out.h> <FunctionName>" >&2
    exit 2
fi
src=$1 out=$2 fn=$3

command -v glslangValidator >/dev/null || {
    echo "glslangValidator not found (Fedora: dnf install glslang)" >&2; exit 1; }

stage=frag
case "$src" in
    *.vert) stage=vert;;
    *.comp) stage=comp;;
esac

tmp=$(mktemp -d "${SCRATCH:-scratch}/gen_native_spv.XXXXXX")
trap 'tools/cleanup_scratch_path.sh "$tmp"' EXIT

glslangValidator -V --target-env vulkan1.1 -S "$stage" "$src" -o "$tmp/out.spv"

python3 - "$src" "$tmp/out.spv" "$out" "$fn" <<'PY'
import struct, sys
src, spv, out, fn = sys.argv[1:5]
data = open(spv, 'rb').read()
assert len(data) % 4 == 0, "SPIR-V is not a whole number of words"
words = struct.unpack('<%dI' % (len(data) // 4), data)
assert words[0] == 0x07230203, "not a little-endian SPIR-V module"
body = ','.join('0x%08xu' % w for w in words)
with open(out, 'w') as f:
    f.write(f"""// GENERATED from {src} by tools/gen_native_spv.sh -- do not edit.
// Regenerate: tools/gen_native_spv.sh {src} {out} {fn}
#pragma once
#include <cstdint>
#include <vector>

namespace gears::native {{
inline const std::vector<uint32_t>& {fn}() {{
    static const std::vector<uint32_t> code = {{{body}}};
    return code;
}}
}} // namespace gears::native
""")
print(f"{out}: {len(words)} SPIR-V words from {src}")
PY

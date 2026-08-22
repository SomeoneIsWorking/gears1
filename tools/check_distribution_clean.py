#!/usr/bin/env python3
"""Refuse tracked proprietary artifacts and unverifiable generated content."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


FORBIDDEN_PREFIXES = (
    "modules/",
    "native/ue3/",
    "roms/",
    "scratch/",
    "shaders/local/",
    "shaders/shareable/",
)

FORBIDDEN_NAMES = {
    "1",
    "ppc_func_mapping.cpp",
    "ppc_recomp_shared.h",
    "tools/prepare_overrides.py",
    "tools/prepare_ue3_core.py",
    "runtime/shaders/movie_yuv.frag",
    "runtime/shaders/scene_gamma.frag",
    "runtime/shaders/uber_post_blend.frag",
    "runtime/shaders/base_pass_lightmap.frag",
    "runtime/shaders/base_pass_lightmap_blend.frag",
    "runtime/shaders/base_pass_lightmap_spec.frag",
}

FORBIDDEN_SUFFIXES = (
    ".bik",
    ".gfr",
    ".iso",
    ".pak",
    ".rom",
    ".upk",
    ".xex",
    ".xiso",
    ".xma",
    ".xpso",
    ".xsh",
    ".xtr",
    ".xxx",
)

FORBIDDEN_TEXT = (
    ("external UE3 source input", re.compile(r"GEARS_UE3_SRC")),
    ("private UE3 source download", re.compile(r"CodeRedModding/UnrealEngine3")),
    ("private UE3 source tree", re.compile(r"Development/Src/(?:Core|Engine|Launch)")),
    ("private UE3 implementation symbol", re.compile(r"FSceneRenderer::RenderFog")),
)

HISTORY_TEXT_LITERALS = (
    ("external UE3 source input", "GEARS_UE3_SRC"),
    ("private UE3 source download", "CodeRedModding/UnrealEngine3"),
    ("private UE3 source tree", "Development/Src/Engine"),
    ("private UE3 implementation symbol", "FSceneRenderer::RenderFog"),
)

# The checker contains its own negative fixtures. The legacy environment loader
# still accepts and ignores the retired variable so old local .env files remain
# harmless; no build target consumes it. Exempt these files from both content
# and pickaxe history scans so the gate tests external use, not its own pattern
# definitions.
TEXT_SCAN_EXEMPT = {
    "tools/check_distribution_clean.py",
    "tools/env.sh",
}

GENERATED_RECOMP = re.compile(r"(?:^|/)ppc_recomp\.\d+\.cpp$")
GENERATED_SPIRV = re.compile(r"(?:^|/)[^/]*_spv\.h$")
SPIRV_PROVENANCE = re.compile(
    r"\A// GENERATED from ([^\r\n]+) by tools/gen_native_spv\.py -- do not edit\."
)
MAX_MARKDOWN_BYTES = 128 * 1024
EXECUTABLE_MAGICS = (b"\x7fELF", b"MZ", b"PK\x03\x04")


def classify_path(path: str) -> list[str]:
    reasons = []
    if path in FORBIDDEN_NAMES:
        reasons.append("forbidden generated/private-source integration file")
    if path.startswith(FORBIDDEN_PREFIXES):
        reasons.append("game-derived or private-source directory")
    if path.lower().endswith(FORBIDDEN_SUFFIXES):
        reasons.append("game/cache artifact extension")
    if GENERATED_RECOMP.search(path):
        reasons.append("generated recompiled title body")
    return reasons


def classify_text(path: str, text: str) -> list[str]:
    reasons = [f"{label}: {pattern.pattern}" for label, pattern in FORBIDDEN_TEXT
               if pattern.search(text)]
    if path.endswith(".md") and len(text.encode("utf-8")) > MAX_MARKDOWN_BYTES:
        reasons.append("oversized forensic document; retain a concise behavioral summary")
    return reasons


def classify_bytes(path: str, data: bytes) -> list[str]:
    reasons = []
    if b"\0" in data:
        reasons.append("tracked binary/NUL content")
    if data.startswith(EXECUTABLE_MAGICS):
        reasons.append("tracked executable/archive magic")
    try:
        data.decode("utf-8")
    except UnicodeDecodeError:
        reasons.append("tracked non-UTF-8 content")
    return reasons


def spirv_source(path: str, text: str) -> str | None:
    if not GENERATED_SPIRV.search(path):
        return None
    match = SPIRV_PROVENANCE.search(text)
    return match.group(1) if match else ""


def tracked_paths(root: Path) -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", "-z"], cwd=root, check=True, capture_output=True
    )
    return [item.decode("utf-8", "surrogateescape")
            for item in result.stdout.split(b"\0") if item]


def scan(root: Path) -> list[tuple[str, str]]:
    failures = []
    for relative in tracked_paths(root):
        path = root / relative
        if not path.exists():
            continue
        for reason in classify_path(relative):
            failures.append((relative, reason))

        if not path.is_file():
            continue
        try:
            data = path.read_bytes()
        except OSError as error:
            failures.append((relative, f"cannot read tracked file: {error}"))
            continue
        for reason in classify_bytes(relative, data):
            failures.append((relative, reason))
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            continue

        source = spirv_source(relative, text)
        if source is not None:
            if not source:
                failures.append((relative, "generated SPIR-V lacks source provenance"))
            else:
                source_path = root / source
                source_reasons = classify_path(source)
                if source_reasons or not source_path.is_file():
                    failures.append(
                        (relative, f"generated SPIR-V source is absent or forbidden: {source}")
                    )
        if relative not in TEXT_SCAN_EXEMPT:
            for reason in classify_text(relative, text):
                failures.append((relative, reason))
    return failures


def history_failures(root: Path) -> list[tuple[str, str]]:
    result = subprocess.run(
        ["git", "log", "--all", "--name-only", "--pretty=format:"],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
    )
    failures = []
    for path in sorted(set(result.stdout.splitlines())):
        for reason in classify_path(path):
            failures.append((path, f"history: {reason}"))

    for label, literal in HISTORY_TEXT_LITERALS:
        pathspec = [".", *(f":(exclude){path}" for path in sorted(TEXT_SCAN_EXEMPT))]
        probe = subprocess.run(
            ["git", "log", "--all", "-S", literal, "--format=%H", "--", *pathspec],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
        )
        commits = sorted(set(probe.stdout.splitlines()))
        if commits:
            failures.append((commits[0], f"history contains {label} ({len(commits)} commit(s))"))
    return failures


def selftest() -> int:
    assert not classify_path("runtime/input.cpp")
    assert classify_path("scratch/ppc/ppc_recomp.1.cpp")
    assert classify_path("modules/title/executable_addr_flags.bin")
    assert classify_path("runtime/default.xex")
    assert classify_path("generated/ppc_recomp.42.cpp")
    assert classify_text("CMakeLists.txt", "set(GEARS_UE3_SRC /private)")
    assert classify_text(
        "doc.md", "Development/Src/Engine is required to build this target"
    )
    assert not classify_text(
        "doc.md", "The engine implements observable package behavior independently"
    )
    assert classify_bytes("runtime/blob.bin", b"MZ\0game")
    assert not classify_bytes("runtime/input.cpp", b"int input = 0;\n")
    assert spirv_source(
        "tests/test_spv.h",
        "// GENERATED from tests/test.frag by tools/gen_native_spv.py -- do not edit.\n",
    ) == "tests/test.frag"
    assert spirv_source("tests/test_spv.h", "#pragma once\n") == ""
    print("clean-distribution checker selftest passed: text, binary, path, and "
          "generated-provenance controls exercised")
    return 0


def main(argv: list[str]) -> int:
    if argv[1:] == ["--selftest"]:
        return selftest()
    if argv[1:] not in ([], ["--history"]):
        print(f"usage: {argv[0]} [--selftest|--history]", file=sys.stderr)
        return 2

    root = Path(__file__).resolve().parents[1]
    failures = history_failures(root) if argv[1:] == ["--history"] else scan(root)
    if failures:
        for path, reason in failures:
            print(f"REFUSING: {path}: {reason}", file=sys.stderr)
        scope = "history" if argv[1:] == ["--history"] else "tracked tip"
        print(f"clean distribution {scope} gate failed: {len(failures)} finding(s)",
              file=sys.stderr)
        return 1

    scope = "history" if argv[1:] == ["--history"] else "tracked tip"
    print(f"clean distribution gate passed: {scope} contains no forbidden "
          "proprietary/generated artifacts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

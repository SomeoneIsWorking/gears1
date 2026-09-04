#!/usr/bin/env python3
"""Non-mutating clang-format and clang-tidy gate for first-party C++."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
CPP_ROOTS = ("runtime", "tests", "tools", "xenia_gpu")
EXCLUDED_PARTS = {"build", "extern", "scratch", ".git", ".venv", "__pycache__"}
MAINTAINED_FILES = tuple(
    Path(path)
    for path in (
        "runtime/byte_order.h",
        "runtime/crt_printf.cpp",
        "runtime/fault_report.cpp",
        "runtime/fault_report.h",
        "runtime/frame_capture.h",
        "runtime/gpu_packet_memory.cpp",
        "runtime/guest_backtrace.cpp",
        "runtime/guest_backtrace.h",
        "runtime/guest_clock.cpp",
        "runtime/guest_dirty_pages.h",
        "runtime/guest_memory.cpp",
        "runtime/guest_memory.h",
        "runtime/guest_texture_hash.cpp",
        "runtime/guest_thread.cpp",
        "runtime/host_time_zone.h",
        "runtime/kernel_config.cpp",
        "runtime/kernel_dispatcher.cpp",
        "runtime/kernel_events.cpp",
        "runtime/kernel_file.cpp",
        "runtime/kernel_memory.cpp",
        "runtime/kernel_misc.cpp",
        "runtime/kernel_object_api.cpp",
        "runtime/kernel_objects.cpp",
        "runtime/kernel_rtl.cpp",
        "runtime/kernel_spinlock.cpp",
        "runtime/kernel_sync.cpp",
        "runtime/kernel_thread.cpp",
        "runtime/kernel_time.cpp",
        "runtime/kernel_timer.cpp",
        "runtime/kernel_video.cpp",
        "runtime/missing_x360port_executor.h",
        "runtime/pm4_trace.cpp",
        "runtime/title_profile.cpp",
        "runtime/title_profile.h",
        "runtime/titles/gears1/audio_mix.cpp",
        "runtime/vd_null_gpu.cpp",
        "runtime/wait_probe.cpp",
        "runtime/wait_probe.h",
        "runtime/xam_loader.cpp",
        "runtime/xam_notify.cpp",
        "runtime/xam_overlapped.cpp",
        "runtime/xam_user.cpp",
        "runtime/xaudio_null.cpp",
        "runtime/xconfig.cpp",
        "runtime/xma.cpp",
        "runtime/xma.h",
        "runtime/xma_context.cpp",
        "runtime/xnet_null.cpp",
        "tests/test_title_profile.cpp",
        "tools/heap_replay.cpp",
        "tools/system_constants/main.cpp",
        "tools/xenos_translate/main.cpp",
        "tools/xma_replay.cpp",
        "xenia_gpu/xenia_host_shim.cpp",
    )
)


def find_tool(name: str, override: str | None = None, finder=shutil.which) -> str:
    candidate = override or finder(name)
    if not candidate:
        raise RuntimeError(f"{name} is not installed")
    return candidate


def is_generated_source(path: Path) -> bool:
    try:
        prefix = path.read_text(encoding="utf-8")[:512]
    except (OSError, UnicodeDecodeError):
        return False
    return "GENERATED from " in prefix or "generated file -- do not edit" in prefix.lower()


def first_party_cpp(root: Path) -> list[Path]:
    files: list[Path] = []
    for source_root in CPP_ROOTS:
        directory = root / source_root
        if not directory.is_dir():
            continue
        for path in directory.rglob("*"):
            relative = path.relative_to(root)
            if any(part in EXCLUDED_PARTS for part in relative.parts):
                continue
            if path.is_file() and path.suffix.lower() in CPP_SUFFIXES and not is_generated_source(path):
                files.append(relative)
    return sorted(files)


def compile_database_sources(build_dir: Path) -> set[Path]:
    database = build_dir / "compile_commands.json"
    if not database.is_file():
        raise RuntimeError(
            f"{database} is missing; configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
        )
    try:
        entries = json.loads(database.read_text(encoding="utf-8"))
        sources: set[Path] = set()
        for entry in entries:
            source = Path(entry["file"])
            if not source.is_absolute():
                source = Path(entry["directory"]) / source
            sources.add(source.resolve())
        return sources
    except (OSError, KeyError, TypeError, ValueError) as error:
        raise RuntimeError(f"{database} is invalid: {error}") from error


def selected_tidy_units(root: Path, database_sources: set[Path]) -> list[Path]:
    units = [
        path
        for path in MAINTAINED_FILES
        if path.suffix.lower() in {".c", ".cc", ".cpp", ".cxx"}
        and (root / path).resolve() in database_sources
    ]
    if not units:
        raise RuntimeError("compile database contains no first-party translation units")
    return units


def run(command: list[str], root: Path) -> None:
    subprocess.run(command, cwd=root, check=True)


def selftest() -> int:
    fake = lambda name: f"/tools/{name}" if name != "missing" else None
    assert find_tool("clang-format", finder=fake) == "/tools/clang-format"
    try:
        find_tool("missing", finder=fake)
    except RuntimeError:
        pass
    else:
        raise AssertionError("missing tools must be refused")

    root = Path(__file__).resolve().parents[1]
    discovered = first_party_cpp(root)
    assert Path("runtime/byte_order.h") in discovered
    assert Path("runtime/wait_probe.cpp") in discovered
    assert Path("tools/heap_replay.cpp") in discovered
    assert all((root / path).is_file() for path in MAINTAINED_FILES)
    assert not any(is_generated_source(root / path) for path in MAINTAINED_FILES)
    selected = selected_tidy_units(root, {(root / "runtime/wait_probe.cpp").resolve()})
    assert selected == [Path("runtime/wait_probe.cpp")]
    print(
        f"C++ quality checker selftest passed: discovered {len(discovered)} current "
        f"first-party files and validated {len(MAINTAINED_FILES)} maintained files; "
        "missing-tool and compiled-unit refusals exercised"
    )
    return 0


def main(argv: list[str]) -> int:
    if argv[1:] == ["--selftest"]:
        return selftest()
    if len(argv) > 2:
        print(f"usage: {argv[0]} [build-dir]", file=sys.stderr)
        return 2

    root = Path(__file__).resolve().parents[1]
    build_dir = Path(argv[1]) if len(argv) == 2 else Path(
        os.environ.get("GEARS_BUILD_DIR", "build/release")
    )
    if not build_dir.is_absolute():
        build_dir = root / build_dir

    try:
        clang_format = find_tool("clang-format", os.environ.get("CLANG_FORMAT"))
        clang_tidy = find_tool("clang-tidy", os.environ.get("CLANG_TIDY"))
        clang_cxx = find_tool("clang++", os.environ.get("CLANG_CXX"))
        formatted = list(MAINTAINED_FILES)
        if not formatted:
            raise RuntimeError("no first-party C++ files discovered")
        missing = [path for path in formatted if not (root / path).is_file()]
        if missing:
            raise RuntimeError(
                "maintained C++ manifest contains missing files: "
                + ", ".join(map(str, missing))
            )
        database_sources = compile_database_sources(build_dir)
        tidy_units = selected_tidy_units(root, database_sources)
    except RuntimeError as error:
        print(f"REFUSING: {error}", file=sys.stderr)
        return 1

    run([clang_format, "--dry-run", "--Werror", *map(str, formatted)], root)
    resource_dir = subprocess.run(
        [clang_cxx, "-print-resource-dir"], check=True, text=True, capture_output=True
    ).stdout.strip()
    run(
        [
            clang_tidy,
            "-p",
            str(build_dir),
            f"--extra-arg=-resource-dir={resource_dir}",
            "--quiet",
            *map(str, tidy_units),
        ],
        root,
    )
    print(
        f"C++ quality gate passed: formatted {len(formatted)} first-party files; "
        f"linted {len(tidy_units)} compiled first-party units"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

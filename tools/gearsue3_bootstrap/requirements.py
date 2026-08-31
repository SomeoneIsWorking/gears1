"""Host prerequisite discovery with actionable, non-privileged refusals."""

from __future__ import annotations

import os
import platform
import shutil
from collections.abc import Callable, Mapping
from pathlib import Path


class RequirementError(RuntimeError):
    """The host lacks a tool required to prepare the shipping product."""


def _linux_distribution(os_release: Path = Path("/etc/os-release")) -> str:
    try:
        lines = os_release.read_text(encoding="utf-8").splitlines()
    except OSError:
        return "unknown"
    values: dict[str, str] = {}
    for line in lines:
        name, separator, value = line.partition("=")
        if separator:
            values[name] = value.strip().strip('"')
    return f"{values.get('ID', '')} {values.get('ID_LIKE', '')}".lower()


def package_command(system: str | None = None, distribution: str | None = None) -> str:
    host = platform.system() if system is None else system
    distro = _linux_distribution() if distribution is None else distribution.lower()
    if host == "Darwin":
        return "brew install cmake ninja pkg-config sdl3 molten-vk"
    if host == "Windows":
        return (
            "winget install Kitware.CMake Ninja-build.Ninja; then install the "
            "Desktop development with C++ workload, SDL3, and the Vulkan SDK"
        )
    if "fedora" in distro or "rhel" in distro or "centos" in distro:
        return (
            "sudo dnf install cmake ninja-build make pkgconf-pkg-config gcc gcc-c++ SDL3-devel "
            "vulkan-loader-devel vulkan-headers"
        )
    if "ubuntu" in distro or "debian" in distro:
        return "sudo apt install cmake ninja-build make pkg-config gcc g++ libsdl3-dev libvulkan-dev"
    return (
        "install CMake, Ninja, a C++20 compiler, SDL3 development files, and "
        "Vulkan headers/loader using your platform package manager"
    )


def require_commands(
    environ: Mapping[str, str] | None = None,
    which: Callable[[str], str | None] = shutil.which,
) -> None:
    environment = os.environ if environ is None else environ
    required = ["git", "cmake", "ninja", "make", "pkg-config"]
    configured_c_compiler = environment.get("CC")
    configured_compiler = environment.get("CXX")
    c_compiler_candidates = (
        [configured_c_compiler] if configured_c_compiler else ["cc", "gcc", "clang"]
    )
    compiler_candidates = [configured_compiler] if configured_compiler else ["c++", "g++", "clang++"]
    missing = [name for name in required if which(name) is None]
    if not any(candidate and which(candidate) is not None for candidate in c_compiler_candidates):
        missing.append(configured_c_compiler or "a C compiler")
    if not any(candidate and which(candidate) is not None for candidate in compiler_candidates):
        missing.append(configured_compiler or "a C++ compiler")
    if missing:
        names = ", ".join(missing)
        raise RequirementError(
            f"missing required host tool(s): {names}\nInstall them with:\n  {package_command()}"
        )


def product_dependency_hint() -> str:
    return f"Install the required product dependencies with:\n  {package_command()}"

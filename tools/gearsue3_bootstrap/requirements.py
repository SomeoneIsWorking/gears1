"""Host prerequisite discovery with actionable, non-privileged refusals."""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
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


# The pkg-config modules the product links, each with the package that
# provides it on the supported Linux families.
PRODUCT_PKG_CONFIG_MODULES = ("gtk+-3.0", "sdl2", "liblz4", "x11-xcb", "fontconfig")
FEDORA_PRODUCT_PACKAGES = "gtk3-devel SDL2-devel lz4-devel libX11-devel fontconfig-devel"
DEBIAN_PRODUCT_PACKAGES = "libgtk-3-dev libsdl2-dev liblz4-dev libx11-xcb-dev libfontconfig-dev"


def package_command(
    system: str | None = None,
    distribution: str | None = None,
    include_archive_tools: bool = False,
) -> str:
    host = platform.system() if system is None else system
    distro = _linux_distribution() if distribution is None else distribution.lower()
    if host != "Linux":
        raise RequirementError(
            f"the Gears of War product has no {host} host yet: x360port provides its "
            "full console only on Linux"
        )
    if "fedora" in distro or "rhel" in distro or "centos" in distro:
        packages = (
            "sudo dnf install cmake ninja-build pkgconf-pkg-config gcc gcc-c++ "
            f"{FEDORA_PRODUCT_PACKAGES}"
        )
        return f"{packages} 7zip" if include_archive_tools else packages
    if "ubuntu" in distro or "debian" in distro:
        packages = f"sudo apt install cmake ninja-build pkg-config g++ {DEBIAN_PRODUCT_PACKAGES}"
        return f"{packages} 7zip" if include_archive_tools else packages
    archive_hint = ", and 7-Zip" if include_archive_tools else ""
    return (
        "install CMake, Ninja, pkg-config, a C++20 compiler, and the development files for "
        f"{', '.join(PRODUCT_PKG_CONFIG_MODULES)}{archive_hint} using your package manager"
    )


def require_commands(
    environ: Mapping[str, str] | None = None,
    which: Callable[[str], str | None] = shutil.which,
) -> None:
    environment = os.environ if environ is None else environ
    required = ["git", "cmake", "ninja", "pkg-config"]
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


def require_archive_command(
    image: Path,
    which: Callable[[str], str | None] = shutil.which,
) -> None:
    if image.suffix.lower() != ".7z" or which("7z") is not None:
        return
    raise RequirementError(
        "missing required host tool(s): 7z\n"
        f"Install it with:\n  {package_command(include_archive_tools=True)}"
    )


def require_pkg_config_modules(
    modules: tuple[str, ...] = PRODUCT_PKG_CONFIG_MODULES,
    exists: Callable[[str], bool] | None = None,
) -> None:
    """Refuse by exact module name before CMake reports the first one it misses."""

    probe = exists or (
        lambda module: subprocess.run(
            ["pkg-config", "--exists", module], check=False
        ).returncode
        == 0
    )
    missing = [module for module in modules if not probe(module)]
    if missing:
        raise RequirementError(
            f"missing development files for: {', '.join(missing)}\n"
            f"Install them with:\n  {package_command()}"
        )

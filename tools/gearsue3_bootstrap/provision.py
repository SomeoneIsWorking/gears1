"""Authenticated-image provisioning and the product build for the launcher."""

from __future__ import annotations

import hashlib
import os
import sys
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO

from tools.gdf_extract import (
    ATTR_DIRECTORY,
    SECTOR,
    GdfError,
    find_base,
    read_volume,
    walk_dir,
)
from tools.title_identity import resolve_image

from .archive import ARCHIVE_SUFFIX, materialize_disc_image
from .paths import build_directory
from .process import CommandRunner
from .profile import TitleProfile
from .requirements import (
    require_archive_command,
    require_commands,
    require_pkg_config_modules,
)

PRODUCT_BUILD = Path("build/product")
PRODUCT_TARGET = "gears1"
LAUNCH_EXECUTABLE = "default.xex"
HASH_CHUNK = 8 << 20
ARCHIVE_IMAGES = Path("scratch/titles/archives")


class ProvisionError(RuntimeError):
    """The selected content cannot produce the exact shipping target."""


@dataclass(frozen=True)
class PreparedTitle:
    """Everything the launcher needs to start the product on one disc."""

    executable: Path
    image: Path
    title_id: str

    def command(self) -> list[str]:
        return [
            os.fspath(self.executable),
            "--image",
            os.fspath(self.image),
            "--title-id",
            self.title_id,
        ]


def disc_executable_digest(disc: BinaryIO) -> str:
    """SHA-256 of the disc's ``default.xex``, read in place from its GDF volume.

    The executable, not the disc image, identifies the revision: two faithful
    dumps of one retail disc may differ outside the game partition.
    """

    base = find_base(disc)
    root_sector, root_size = read_volume(disc, base)
    matches = [
        (sector, size)
        for path, sector, size, attributes in walk_dir(disc, base, root_sector, root_size)
        if path.lower() == LAUNCH_EXECUTABLE and not attributes & ATTR_DIRECTORY
    ]
    if len(matches) != 1:
        raise ProvisionError(
            f"the disc has {len(matches)} root {LAUNCH_EXECUTABLE} entries, not exactly one"
        )
    sector, remaining = matches[0]
    disc.seek(base + sector * SECTOR)
    digest = hashlib.sha256()
    while remaining:
        chunk = disc.read(min(remaining, HASH_CHUNK))
        if not chunk:
            raise ProvisionError(f"the disc ends inside {LAUNCH_EXECUTABLE}")
        digest.update(chunk)
        remaining -= len(chunk)
    return digest.hexdigest()


def authenticate_image(image: Path, profile: TitleProfile) -> None:
    """Refuse every disc but the profile's exact retail executable."""

    try:
        with image.open("rb") as disc:
            measured = disc_executable_digest(disc)
    except GdfError as error:
        raise ProvisionError(f"{image} is not a readable Xbox 360 disc image: {error}") from error
    if measured != profile.identity.xex_sha256:
        raise ProvisionError(
            f"{image} is not the supported {profile.display_name} disc: its "
            f"{LAUNCH_EXECUTABLE} SHA-256 is {measured}, the supported revision is "
            f"{profile.identity.xex_sha256}"
        )


def build_product(
    repo_root: Path,
    environ: Mapping[str, str],
    runner: CommandRunner,
) -> Path:
    """Configure once and build the product target with the host's toolchain."""

    build = build_directory(repo_root, environ.get("GEARS_BUILD_DIR"), repo_root / PRODUCT_BUILD)
    if not (build / "CMakeCache.txt").is_file():
        runner.run(
            [
                "cmake",
                "-S",
                repo_root,
                "-B",
                build,
                "-G",
                "Ninja",
                "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                "-DGEARS_DYNAREC_CONTRACT_ONLY=ON",
                f"-DPython3_EXECUTABLE={sys.executable}",
            ],
            cwd=repo_root,
            environ=environ,
        )
    runner.run(
        ["cmake", "--build", build, "--target", PRODUCT_TARGET],
        cwd=repo_root,
        environ=environ,
    )
    executable = build / PRODUCT_TARGET
    if not executable.is_file():
        raise ProvisionError(f"the build did not produce {executable}")
    return executable


def prepare_title(
    repo_root: Path,
    profile: TitleProfile,
    *,
    image: str | os.PathLike[str] | None = None,
    environ: Mapping[str, str] | None = None,
    env_file: Path | None = None,
    runner: CommandRunner | None = None,
) -> PreparedTitle:
    """Resolve and authenticate the user's disc, then build the product for it."""

    environment = dict(os.environ if environ is None else environ)
    commands = CommandRunner() if runner is None else runner
    resolved = resolve_image(image, repo_root, environment, env_file).path
    require_archive_command(resolved)
    require_commands(environment)
    require_pkg_config_modules()
    if resolved.suffix.lower() == ARCHIVE_SUFFIX:
        resolved = materialize_disc_image(
            resolved, repo_root / ARCHIVE_IMAGES, runner=commands, cwd=repo_root
        )
    authenticate_image(resolved, profile)
    executable = build_product(repo_root, environment, commands)
    return PreparedTitle(executable, resolved, profile.identity.title_id)

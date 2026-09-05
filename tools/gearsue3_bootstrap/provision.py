"""Authenticated-image provisioning boundary for the x360port product."""

from __future__ import annotations

import os
from collections.abc import Mapping
from pathlib import Path

from .process import CommandRunner
from .profile import TitleProfile


class ProvisionError(RuntimeError):
    """The selected content cannot produce the exact shipping target."""


def prepare_title(
    repo_root: Path,
    profile: TitleProfile,
    *,
    image: str | os.PathLike[str] | None = None,
    environ: Mapping[str, str] | None = None,
    env_file: Path | None = None,
    runner: CommandRunner | None = None,
) -> None:
    """Refuse at the one missing product-composition boundary.

    Exact disc resolution, GDF extraction, title fingerprinting, checked-XEX
    metadata parsing, and exact-revision validation remain independently tested
    owners. Product composition resumes here only when x360port exposes its
    authenticated Xenia executor; no offline guest-code generation is allowed.
    """
    del profile, image, environ, env_file, runner
    expected = (repo_root / "../../shared/x360port").resolve()
    raise ProvisionError(
        "Gears full-image adapter and runtime services are missing over the "
        f"authenticated Xenia dynarec integration at {expected}. The retired generated-code "
        "product cannot be generated, built, or selected."
    )

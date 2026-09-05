"""Fail-closed contracts for consumed Git source checkouts."""

from __future__ import annotations

import re
import shutil
from dataclasses import dataclass
from pathlib import Path

from .process import CommandError, CommandRunner

FULL_REVISION = re.compile(r"[0-9a-f]{40}")


class DependencyCheckoutError(RuntimeError):
    """A consumed dependency does not match its immutable clean contract."""


@dataclass(frozen=True)
class GitCheckout:
    root: Path
    revision: str


def require_git_checkout(
    root: Path,
    expected_revision: str,
    label: str,
    *,
    required_file: Path | None = None,
    runner: CommandRunner | None = None,
    git: str | None = None,
) -> GitCheckout:
    """Return an exact clean checkout or raise with the violated invariant."""

    resolved = root.resolve()
    if not resolved.is_dir():
        raise DependencyCheckoutError(f"{label} checkout is missing: {resolved}")
    if required_file is not None and not (resolved / required_file).is_file():
        raise DependencyCheckoutError(
            f"{label} checkout lacks required file {required_file}: {resolved}"
        )
    if FULL_REVISION.fullmatch(expected_revision) is None:
        raise DependencyCheckoutError(
            f"{label} required revision is not an immutable 40-digit SHA: "
            f"{expected_revision!r}"
        )

    selected_git = git or shutil.which("git")
    if selected_git is None:
        raise DependencyCheckoutError("required program is missing from PATH: git")
    commands = runner or CommandRunner()
    try:
        observed = commands.capture(
            [selected_git, "-C", resolved, "rev-parse", "--verify", "HEAD^{commit}"],
            cwd=resolved,
        )
    except CommandError as error:
        raise DependencyCheckoutError(
            f"{label} is not a readable Git checkout at {resolved}: {error}"
        ) from error
    if observed != expected_revision:
        raise DependencyCheckoutError(
            f"{label} requires revision {expected_revision}, observed {observed} "
            f"at {resolved}"
        )

    try:
        dirty = commands.capture(
            [
                selected_git,
                "-C",
                resolved,
                "status",
                "--porcelain=v1",
                "--untracked-files=all",
                "--ignore-submodules=none",
            ],
            cwd=resolved,
        )
    except CommandError as error:
        raise DependencyCheckoutError(
            f"{label} cleanliness could not be inspected at {resolved}: {error}"
        ) from error
    if dirty:
        raise DependencyCheckoutError(
            f"{label} must be clean at revision {expected_revision}; dirty entries:\n{dirty}"
        )
    return GitCheckout(root=resolved, revision=observed)

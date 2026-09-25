"""Shipping command-line contract and bootstrap composition."""

from __future__ import annotations

import os
import sys
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

from tools.title_identity import IdentityError

from .archive import ArchiveError
from .environment import EnvironmentError, environment_file, load_environment
from .paths import BuildPathError
from .process import CommandError
from .profile import ProfileError, load_profile
from .provision import ProvisionError, prepare_title
from .requirements import RequirementError

USAGE = """Usage: ./run.sh [--iso <path>] [--prepare]

Play Gears of War from your own disc image. The disc is found from --iso, then
GEARS_ISO in the environment or .env, then the one image or 7z archive in roms/.
Only the supported retail revision is accepted.

The game serves its loopback-only control channel on port 32125, so its
performance and state can be read while you play; GEARS_CONTROL_PORT=<port>
in the environment or .env picks another port.

Options:
  --iso <path>  the disc image or 7z archive to play
  --prepare     check the disc and build the game, but do not start it
  -h, --help    show this text
"""


MAX_PORT = 65535
DEFAULT_CONTROL_PORT = 32125


class CliError(RuntimeError):
    """The shipping command line is incomplete or contradictory."""


@dataclass(frozen=True)
class LaunchOptions:
    image: str | None = None
    prepare_only: bool = False
    show_help: bool = False


def parse_arguments(arguments: Sequence[str]) -> LaunchOptions:
    image: str | None = None
    prepare_only = False
    show_help = False
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument in {"-h", "--help"}:
            show_help = True
        elif argument == "--prepare":
            prepare_only = True
        elif argument == "--iso":
            if index + 1 >= len(arguments):
                raise CliError("--iso requires a value")
            index += 1
            image = arguments[index]
        else:
            raise CliError(f"unknown option {argument!r} (try --help)")
        index += 1
    return LaunchOptions(image=image, prepare_only=prepare_only, show_help=show_help)


def control_port_arguments(environ: Mapping[str, str]) -> list[str]:
    """The product arguments that serve the control channel, on GEARS_CONTROL_PORT or the default."""

    value = environ.get("GEARS_CONTROL_PORT", str(DEFAULT_CONTROL_PORT))
    if not value.isdigit() or not 1 <= int(value) <= MAX_PORT:
        raise CliError(f"GEARS_CONTROL_PORT must be a port from 1 to {MAX_PORT}, not {value!r}")
    return ["--control-port", str(int(value))]


def main(
    arguments: Sequence[str] | None = None,
    repo_root: Path | None = None,
    execute: Callable[[list[str]], None] | None = None,
) -> int:
    root = Path(__file__).resolve().parents[2] if repo_root is None else repo_root.resolve()
    options = parse_arguments(list(sys.argv[1:] if arguments is None else arguments))
    if options.show_help:
        print(USAGE)
        return 0
    selected_environment_file = environment_file(root)
    environ = load_environment(root, env_file=selected_environment_file)
    control = control_port_arguments(environ)
    prepared = prepare_title(
        root,
        load_profile(root),
        image=options.image,
        environ=environ,
        env_file=selected_environment_file,
    )
    command = prepared.command() + control
    if options.prepare_only:
        print(f"bootstrap: ready: {' '.join(command)}")
        return 0
    (execute or _replace_process)(command)
    raise AssertionError("the product launch returned")


def _replace_process(command: list[str]) -> None:
    sys.stdout.flush()
    sys.stderr.flush()
    os.execv(command[0], command)


def entrypoint(arguments: list[str] | None = None) -> int:
    try:
        return main(arguments)
    except (
        ArchiveError,
        BuildPathError,
        CliError,
        CommandError,
        EnvironmentError,
        IdentityError,
        ProfileError,
        ProvisionError,
        RequirementError,
    ) as error:
        print(f"bootstrap: REFUSING: {error}", file=sys.stderr)
        return 2

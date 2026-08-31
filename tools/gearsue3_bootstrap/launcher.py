"""Shipping command-line contract and bootstrap composition."""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass, field
from pathlib import Path

from .environment import EnvironmentError, environment_file, load_environment
from .process import run_logged_child
from .paths import BuildPathError, build_directory
from .profile import ProfileError, load_profile
from .provision import PreparedTitle, ProvisionError, prepare_title

USAGE = """Usage: ./run.sh [options] [-- extra runtime arguments]

Build the current GearsUE3 product from a user-owned Gears of War disc image
and launch it. Select the image with GEARS_ISO, .env, or one file in roms/.

Options:
  --headless          no window (GEARS_NO_WINDOW=1); measurement runs
  --no-build          launch an already prepared GEARS_GAME_DIR/GEARS_BUILD_DIR
  --prepare           provision and build, but do not launch
  --iso <path>        explicit user-owned disc image
  --log <path>        tee runtime output (default scratch/logs/run.log)
  --script <steps>    scripted pad input
  --http-port <port>  loopback debug API (default 32123; 0 disables)
  --present-dump N    write the next N presented frames after frame 300
  --menu-walk         title-profile route from boot into Act 1
  -h, --help          show this text
"""


class CliError(RuntimeError):
    """The shipping command line is incomplete or contradictory."""


@dataclass
class LaunchOptions:
    headless: bool = False
    no_build: bool = False
    prepare_only: bool = False
    image: str | None = None
    log_path: Path = Path("scratch/logs/run.log")
    input_script: str | None = None
    http_port: str = "32123"
    present_dump: str | None = None
    runtime_arguments: list[str] = field(default_factory=list)
    show_help: bool = False


def _value(arguments: list[str], index: int, option: str) -> tuple[str, int]:
    if index + 1 >= len(arguments):
        raise CliError(f"{option} requires a value")
    return arguments[index + 1], index + 2


def parse_arguments(
    arguments: list[str], menu_walk: str, default_http_port: str = "32123"
) -> LaunchOptions:
    options = LaunchOptions(http_port=default_http_port)
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == "--":
            options.runtime_arguments.extend(arguments[index + 1 :])
            break
        if argument in {"-h", "--help"}:
            options.show_help = True
            index += 1
        elif argument == "--headless":
            options.headless = True
            index += 1
        elif argument == "--no-build":
            options.no_build = True
            index += 1
        elif argument == "--prepare":
            options.prepare_only = True
            index += 1
        elif argument == "--menu-walk":
            options.input_script = menu_walk
            index += 1
        elif argument in {"--iso", "--log", "--script", "--http-port", "--present-dump"}:
            selected, index = _value(arguments, index, argument)
            if argument == "--iso":
                options.image = selected
            elif argument == "--log":
                options.log_path = Path(selected)
            elif argument == "--script":
                options.input_script = selected
            elif argument == "--http-port":
                options.http_port = selected
            else:
                options.present_dump = selected
        elif argument.startswith("-"):
            raise CliError(f"unknown option {argument!r} (try --help)")
        else:
            options.runtime_arguments.extend(arguments[index:])
            break
    if options.no_build and options.prepare_only:
        raise CliError("--no-build and --prepare are mutually exclusive")
    if not options.http_port.isdecimal() or not 0 <= int(options.http_port) <= 65535:
        raise CliError("--http-port must be an integer from 0 through 65535")
    if options.present_dump is not None and (
        not options.present_dump.isdecimal() or int(options.present_dump) <= 0
    ):
        raise CliError("--present-dump must be a positive integer")
    return options


def _existing_title(repo_root: Path, environment: dict[str, str]) -> PreparedTitle:
    game_dir = Path(environment.get("GEARS_GAME_DIR", repo_root / "scratch/game")).resolve()
    build_dir = build_directory(
        repo_root,
        environment.get("GEARS_BUILD_DIR"),
        repo_root / "build/release",
    )
    executable = game_dir / "default.xex"
    runtime = build_dir / "runtime/gears1"
    if not executable.is_file():
        raise ProvisionError(f"--no-build requires extracted title executable {executable}")
    if not runtime.is_file() or not os.access(runtime, os.X_OK):
        raise ProvisionError(f"--no-build requires built product executable {runtime}")
    return PreparedTitle(
        disc_image=Path(),
        title_root=game_dir.parent,
        game_dir=game_dir,
        ppc_dir=Path(environment.get("GEARS_PPC_DIR", repo_root / "scratch/ppc")),
        build_dir=build_dir,
        executable=executable,
        runtime=runtime,
        identity_path=Path(),
    )


def _launch_environment(
    base: dict[str, str], options: LaunchOptions, repo_root: Path
) -> dict[str, str]:
    environment = dict(base)
    if options.headless:
        environment["GEARS_NO_WINDOW"] = "1"
    if options.input_script:
        environment["GEARS_INPUT_SCRIPT"] = options.input_script
    environment["GEARS_DEBUG_HTTP_PORT"] = options.http_port
    if options.present_dump is not None:
        environment["GEARS_PRESENT_DUMP"] = options.present_dump
        environment.setdefault("GEARS_PRESENT_DUMP_AT", "300")
        (repo_root / "scratch/screenshots").mkdir(parents=True, exist_ok=True)
    return environment


def main(arguments: list[str] | None = None, repo_root: Path | None = None) -> int:
    root = Path(__file__).resolve().parents[2] if repo_root is None else repo_root.resolve()
    profile = load_profile(root)
    selected_environment_file = environment_file(root)
    environment = load_environment(root, env_file=selected_environment_file)
    options = parse_arguments(
        list(sys.argv[1:] if arguments is None else arguments),
        environment.get("GEARS_MENU_WALK", profile.navigation.menu_walk),
        environment.get("GEARS_DEBUG_HTTP_PORT", "32123"),
    )
    if options.show_help:
        print(USAGE)
        return 0

    target = (
        _existing_title(root, environment)
        if options.no_build
        else prepare_title(
            root,
            profile,
            image=options.image,
            environ=environment,
            env_file=selected_environment_file,
        )
    )
    print(
        f"bootstrap: prepared {profile.display_name} at {target.title_root.relative_to(root)}",
        file=sys.stderr,
    )
    if options.prepare_only:
        return 0

    launch_environment = _launch_environment(environment, options, root)
    command = [target.runtime, target.executable, target.game_dir, *options.runtime_arguments]
    log_path = options.log_path if options.log_path.is_absolute() else root / options.log_path
    print(f"bootstrap: launching {target.runtime} (log: {log_path})", file=sys.stderr)
    if options.http_port != "0":
        print(
            f"bootstrap: interactive debug API http://127.0.0.1:{options.http_port}",
            file=sys.stderr,
        )
    return run_logged_child(
        command,
        cwd=root,
        environ=launch_environment,
        log_path=log_path,
    )


def entrypoint(arguments: list[str] | None = None) -> int:
    try:
        return main(arguments)
    except (
        BuildPathError,
        CliError,
        EnvironmentError,
        ProfileError,
        ProvisionError,
    ) as error:
        print(f"bootstrap: REFUSING: {error}", file=sys.stderr)
        return 2

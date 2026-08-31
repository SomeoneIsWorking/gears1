"""Content-addressed disc extraction, title generation, and product build."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import sys
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path

from tools import gdf_extract, merge_switch_tables, title_identity

from . import archive
from .paths import BuildPathError, build_directory
from .process import CommandError, CommandRunner
from .profile import TitleProfile
from .requirements import (
    RequirementError,
    product_dependency_hint,
    require_archive_command,
    require_commands,
)


class ProvisionError(RuntimeError):
    """The selected content cannot produce the exact shipping target."""


@dataclass(frozen=True)
class PreparedTitle:
    disc_image: Path
    title_root: Path
    game_dir: Path
    ppc_dir: Path
    build_dir: Path
    executable: Path
    runtime: Path
    identity_path: Path


def _json_write(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def _json_read(path: Path) -> object | None:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None
    except (OSError, json.JSONDecodeError) as error:
        raise ProvisionError(f"cannot read provisioning stamp {path}: {error}") from error


def _hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _require_within(path: Path, root: Path, description: str) -> Path:
    resolved_root = root.resolve()
    resolved = path.resolve()
    if resolved == resolved_root or resolved_root not in resolved.parents:
        raise ProvisionError(f"{description} escapes its scoped root: {path}")
    return resolved


def _reset_generated_directory(path: Path, title_root: Path) -> None:
    target = _require_within(path, title_root, "generated output")
    if target.is_symlink():
        raise ProvisionError(f"generated output is a symlink: {target}")
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)


def _submodules_ready(repo_root: Path) -> bool:
    required = (
        repo_root / "extern/XenonRecomp/CMakeLists.txt",
        repo_root / "extern/xenia/src/xenia/gpu",
        repo_root / "extern/ffmpeg-xmaframes/configure",
    )
    return all(path.exists() for path in required)


def _ensure_submodules(repo_root: Path, runner: CommandRunner) -> None:
    if _submodules_ready(repo_root):
        return
    runner.run(
        ["git", "submodule", "update", "--init", "--recursive"], cwd=repo_root
    )
    if not _submodules_ready(repo_root):
        raise ProvisionError("pinned submodules remain incomplete after initialization")


def _build_recompiler(repo_root: Path, runner: CommandRunner) -> Path:
    build_dir = repo_root / "build/deps/xenonrecomp"
    runner.run(
        [
            "cmake",
            "-S",
            repo_root / "extern/XenonRecomp",
            "-B",
            build_dir,
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DBUILD_TESTING=OFF",
        ],
        cwd=repo_root,
    )
    runner.run(
        [
            "cmake",
            "--build",
            build_dir,
            "--target",
            "XenonRecomp",
            "XenonAnalyse",
            "xex-inspect",
        ],
        cwd=repo_root,
    )
    return build_dir


def _disc_entries(image: Path) -> tuple[int, list[tuple[str, int, int, int]]]:
    with image.open("rb") as source:
        base = gdf_extract.find_base(source)
        root_sector, root_size = gdf_extract.read_volume(source, base)
        return base, gdf_extract.walk_dir(source, base, root_sector, root_size)


def _extraction_manifest(entries: list[tuple[str, int, int, int]]) -> dict[str, object]:
    files = [
        {"path": path, "size": size}
        for path, _start, size, attributes in sorted(entries)
        if not attributes & gdf_extract.ATTR_DIRECTORY
    ]
    return {"schema": 1, "files": files}


def _extraction_complete(game_dir: Path, manifest: object) -> bool:
    if not isinstance(manifest, dict) or manifest.get("schema") != 1:
        return False
    files = manifest.get("files")
    if not isinstance(files, list) or not files:
        return False
    for entry in files:
        if not isinstance(entry, dict):
            return False
        relative = entry.get("path")
        size = entry.get("size")
        if not isinstance(relative, str) or not isinstance(size, int):
            return False
        candidate = game_dir / relative
        if candidate.is_symlink() or not candidate.is_file():
            return False
        if candidate.stat().st_size != size:
            return False
    return True


def _extract_disc(image: Path, game_dir: Path) -> None:
    stamp = game_dir.parent / "extraction.json"
    existing = _json_read(stamp)
    if _extraction_complete(game_dir, existing):
        print(f"bootstrap: verified {len(existing['files'])} extracted files", file=sys.stderr)
        return
    base, entries = _disc_entries(image)
    with image.open("rb") as source:
        gdf_extract.extract_all(source, base, entries, game_dir)
    manifest = _extraction_manifest(entries)
    if not _extraction_complete(game_dir, manifest):
        raise ProvisionError("disc extraction completed without a valid complete-install manifest")
    _json_write(stamp, manifest)


def _augment_identity(
    identity: dict[str, object],
    executable: Path,
    repo_root: Path,
    inspector: Path,
) -> dict[str, object]:
    xex = title_identity.fingerprint(executable)
    xex["metadata"] = title_identity.parse_xex_metadata(
        executable,
        repo_root=repo_root,
        xex_inspect=inspector,
    )
    return {**identity, "xex": xex}


def _validate_title(identity: dict[str, object], profile: TitleProfile) -> None:
    xex = identity.get("xex")
    if not isinstance(xex, dict):
        raise ProvisionError("generated identity has no XEX metadata")
    metadata = xex.get("metadata")
    if not isinstance(metadata, dict):
        raise ProvisionError("generated identity has no checked XEX metadata")
    execution = metadata.get("execution")
    if not isinstance(execution, dict):
        raise ProvisionError("generated identity has no XEX execution metadata")
    image = metadata.get("image")
    if not isinstance(image, dict):
        raise ProvisionError("generated identity has no normalized-image metadata")
    expected: dict[str, object] = {
        "title_id": profile.identity.title_id,
        "savegame_id": profile.identity.savegame_id,
        "platform": profile.identity.platform,
        "disc_number": profile.identity.disc_number,
        "disc_count": profile.identity.disc_count,
    }
    mismatches = [
        f"{name}: expected {wanted!r}, observed {execution.get(name)!r}"
        for name, wanted in expected.items()
        if execution.get(name) != wanted
    ]
    if mismatches:
        raise ProvisionError(
            f"selected disc is not the {profile.display_name} title profile: "
            + "; ".join(mismatches)
        )
    revision_mismatches = []
    if xex.get("sha256") != profile.identity.xex_sha256:
        revision_mismatches.append(
            f"xex_sha256: expected {profile.identity.xex_sha256!r}, "
            f"observed {xex.get('sha256')!r}"
        )
    if image.get("sha256") != profile.identity.image_sha256:
        revision_mismatches.append(
            f"image_sha256: expected {profile.identity.image_sha256!r}, "
            f"observed {image.get('sha256')!r}"
        )
    if revision_mismatches:
        raise ProvisionError(
            f"selected disc is an unsupported {profile.display_name} revision: "
            + "; ".join(revision_mismatches)
        )


def _relative_config_path(path: Path, config_dir: Path) -> str:
    return Path(os.path.relpath(path, config_dir)).as_posix()


def _materialize_recompiler_config(
    template: Path,
    destination: Path,
    executable: Path,
    ppc_dir: Path,
    switch_tables: Path,
) -> None:
    text = template.read_text(encoding="utf-8")
    replacements = {
        "file_path": _relative_config_path(executable, destination.parent),
        "out_directory_path": _relative_config_path(ppc_dir, destination.parent),
        "switch_table_file_path": _relative_config_path(
            switch_tables, destination.parent
        ),
    }
    for name, value in replacements.items():
        pattern = re.compile(rf'(?m)^{re.escape(name)}\s*=\s*"[^"]*"\s*$')
        text, count = pattern.subn(f'{name} = "{value}"', text)
        if count != 1:
            raise ProvisionError(
                f"recompiler template must define {name} exactly once; found {count}"
            )
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(f".{destination.name}.{os.getpid()}.tmp")
    temporary.write_text(text, encoding="utf-8")
    os.replace(temporary, destination)


def _generation_inputs(
    identity: dict[str, object],
    profile: TitleProfile,
    repo_root: Path,
    runner: CommandRunner,
) -> dict[str, object]:
    xex = identity["xex"]
    assert isinstance(xex, dict)
    metadata = xex["metadata"]
    assert isinstance(metadata, dict)
    image = metadata["image"]
    assert isinstance(image, dict)
    return {
        "schema": 1,
        "xex_sha256": xex["sha256"],
        "image_sha256": image["sha256"],
        "config_sha256": _hash_file(repo_root / profile.recompiler_template),
        "switch_extra_sha256": _hash_file(repo_root / profile.switch_tables_extra),
        "recompiler_revision": runner.capture(
            ["git", "rev-parse", "HEAD"], cwd=repo_root / "extern/XenonRecomp"
        ),
    }


def _generated_module_complete(ppc_dir: Path, expected: dict[str, object]) -> bool:
    if _json_read(ppc_dir / "provision.json") != expected:
        return False
    return (ppc_dir / "ppc_config.h").is_file() and any(
        ppc_dir.glob("ppc_recomp.*.cpp")
    )


def _generate_title_module(
    repo_root: Path,
    title_root: Path,
    executable: Path,
    ppc_dir: Path,
    identity: dict[str, object],
    profile: TitleProfile,
    recompiler_build: Path,
    runner: CommandRunner,
) -> None:
    expected = _generation_inputs(identity, profile, repo_root, runner)
    if _generated_module_complete(ppc_dir, expected):
        print("bootstrap: generated title module is current", file=sys.stderr)
        return
    config_dir = title_root / "config"
    config_dir.mkdir(parents=True, exist_ok=True)
    generated_switches = config_dir / "gears_switch_tables.generated.toml"
    merged_switches = config_dir / "gears_switch_tables.toml"
    runner.run(
        [
            recompiler_build / "XenonAnalyse/XenonAnalyse",
            executable,
            generated_switches,
        ],
        cwd=repo_root,
    )
    merge_switch_tables.merge_switch_tables(
        generated_switches,
        repo_root / profile.switch_tables_extra,
        merged_switches,
    )
    config_path = config_dir / "gears.toml"
    _materialize_recompiler_config(
        repo_root / profile.recompiler_template,
        config_path,
        executable,
        ppc_dir,
        merged_switches,
    )
    _reset_generated_directory(ppc_dir, title_root)
    runner.run(
        [
            recompiler_build / "XenonRecomp/XenonRecomp",
            config_path,
            repo_root / "extern/XenonRecomp/XenonUtils/ppc_context.h",
        ],
        cwd=repo_root,
    )
    if not (ppc_dir / "ppc_config.h").is_file() or not any(
        ppc_dir.glob("ppc_recomp.*.cpp")
    ):
        raise ProvisionError("XenonRecomp exited successfully but emitted no complete module")
    _json_write(ppc_dir / "provision.json", expected)


def _build_product(
    repo_root: Path,
    game_dir: Path,
    ppc_dir: Path,
    build_dir: Path,
    runner: CommandRunner,
) -> Path:
    configure = [
        "cmake",
        "-S",
        repo_root,
        "-B",
        build_dir,
        "-G",
        "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DGEARS_REQUIRE_PRODUCT_DEPS=ON",
        f"-DGEARS_GAME_DIR={game_dir}",
        f"-DGEARS_PPC_DIR={ppc_dir}",
        f"-DPython3_EXECUTABLE={sys.executable}",
    ]
    try:
        runner.run(configure, cwd=repo_root)
        runner.run(
            ["cmake", "--build", build_dir, "--target", "gears1"], cwd=repo_root
        )
    except CommandError as error:
        raise ProvisionError(f"{error}\n{product_dependency_hint()}") from error
    runtime = build_dir / "runtime/gears1"
    if not runtime.is_file() or not os.access(runtime, os.X_OK):
        raise ProvisionError(f"product build did not produce executable {runtime}")
    return runtime


def prepare_title(
    repo_root: Path,
    profile: TitleProfile,
    *,
    image: str | os.PathLike[str] | None = None,
    environ: Mapping[str, str] | None = None,
    env_file: Path | None = None,
    runner: CommandRunner | None = None,
) -> PreparedTitle:
    environment = dict(os.environ if environ is None else environ)
    command_runner = CommandRunner() if runner is None else runner
    try:
        require_commands(environment)
        try:
            resolved = title_identity.resolve_image(
                image, repo_root, environment, env_file=env_file
            )
        except title_identity.IdentityError as error:
            raise ProvisionError(str(error)) from error
        require_archive_command(resolved.path)
        disc_image = (
            archive.materialize_disc_image(
                resolved.path,
                repo_root / "scratch/archives",
                runner=command_runner,
                cwd=repo_root,
            )
            if resolved.path.suffix.lower() == archive.ARCHIVE_SUFFIX
            else resolved.path
        )
        disc_identity = title_identity.build_identity(disc_image)
        identity_path = title_identity.write_identity(repo_root, disc_identity)
        title_root = identity_path.parent
        game_dir = title_root / "game"
        ppc_dir = title_root / "ppc"
        build_dir = build_directory(
            repo_root,
            environment.get("GEARS_BUILD_DIR"),
            repo_root / "build/titles" / title_root.name / "release",
        )
        _ensure_submodules(repo_root, command_runner)
        recompiler_build = _build_recompiler(repo_root, command_runner)
        _extract_disc(disc_image, game_dir)
        executable = game_dir / "default.xex"
        if not executable.is_file():
            raise ProvisionError("selected disc has no root default.xex")
        identity = _augment_identity(
            disc_identity,
            executable,
            repo_root,
            recompiler_build / "XexInspect/xex-inspect",
        )
        _validate_title(identity, profile)
        identity_path = title_identity.write_identity(repo_root, identity)
        _generate_title_module(
            repo_root,
            title_root,
            executable,
            ppc_dir,
            identity,
            profile,
            recompiler_build,
            command_runner,
        )
        runtime = _build_product(
            repo_root, game_dir, ppc_dir, build_dir, command_runner
        )
    except (
        OSError,
        gdf_extract.GdfError,
        title_identity.IdentityError,
        RequirementError,
        CommandError,
        BuildPathError,
    ) as error:
        raise ProvisionError(str(error)) from error
    return PreparedTitle(
        disc_image=disc_image,
        title_root=title_root,
        game_dir=game_dir,
        ppc_dir=ppc_dir,
        build_dir=build_dir,
        executable=executable,
        runtime=runtime,
        identity_path=identity_path,
    )

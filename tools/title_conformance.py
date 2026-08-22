#!/usr/bin/env python3
"""Report exact-build conformance for the Xenon Gears UE3 runtime.

The tool consumes pairs of locally generated JSON files: a manifest describing
one exact executable build, and results describing evidence-backed gates for
that same manifest. It does not identify a game from a title ID and it never
turns recognition into a compatibility claim.

Faithful compatibility and the per-game 60 fps enhancement are separate
outcomes. The enhancement gate remains mandatory in the evidence schema so it
cannot disappear from reports, but a failed enhancement does not make an
otherwise faithful exact build incompatible.

Usage:
    tools/title_conformance.py check MANIFEST RESULTS [MANIFEST RESULTS ...]
    tools/title_conformance.py check --json MANIFEST RESULTS
    tools/title_conformance.py --selftest

Every evidence record names a local artifact relative to the results file, its
SHA-256 digest, and the measurement source. The artifact may be a log, JSON
report, or other generated measurement; game files and generated recompilation
sources are not inputs to this reporter. Gears 2, 3, and Judgment evidence is
rejected when it names Xenia as its oracle; those titles require an independent
headless invariant, compatibility-renderer A/B, or hardware-derived source.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import sys
import tempfile
from contextlib import redirect_stdout
from dataclasses import dataclass
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 2

GAME_NAMES = {
    "gears1": "Gears of War",
    "gears2": "Gears of War 2",
    "gears3": "Gears of War 3",
    "judgment": "Gears of War: Judgment",
}

COMPATIBILITY_GATES = (
    "identity",
    "recompilation",
    "headless_boot",
    "content_mount",
    "menu",
    "gameplay",
    "renderer_compatibility",
)
PARITY_GATES = (
    "renderer_native_parity",
    "override_ab",
)
ENHANCEMENT_GATES = ("gameplay_60fps",)
ALL_GATES = COMPATIBILITY_GATES + PARITY_GATES + ENHANCEMENT_GATES

STATUSES = {"pass", "fail", "not_applicable"}
HEX32_FIELDS = (
    "title_id",
    "media_id",
    "xex_version",
    "base_version",
)
DIGEST_FIELDS = (
    "xex_sha256",
    "image_sha256",
)


class Refusal(ValueError):
    """The supplied evidence cannot support a conformance report."""


@dataclass(frozen=True)
class CaseReport:
    game: str
    identity: dict[str, Any]
    gates: dict[str, str]
    summaries: dict[str, tuple[str, ...]]
    compatibility_ready: bool
    native_parity: str
    sixty_fps_ready: bool

    def to_json(self) -> dict[str, Any]:
        return {
            "game": self.game,
            "game_name": GAME_NAMES[self.game],
            "identity": self.identity,
            "gates": self.gates,
            "evidence_summaries": {
                gate: list(rows) for gate, rows in self.summaries.items()
            },
            "compatibility_ready": self.compatibility_ready,
            "native_parity": self.native_parity,
            "sixty_fps_ready": self.sixty_fps_ready,
        }


def load_json(path: Path, what: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise Refusal(f"{what} {path}: cannot read: {error}") from error
    except json.JSONDecodeError as error:
        raise Refusal(f"{what} {path}: invalid JSON: {error}") from error
    if not isinstance(value, dict):
        raise Refusal(f"{what} {path}: top level must be an object")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while block := stream.read(1024 * 1024):
                digest.update(block)
    except OSError as error:
        raise Refusal(f"evidence artifact {path}: cannot read: {error}") from error
    return digest.hexdigest()


def require_exact_keys(value: dict[str, Any], required: set[str], where: str) -> None:
    missing = sorted(required - value.keys())
    unknown = sorted(value.keys() - required)
    if missing:
        raise Refusal(f"{where}: missing field(s): {', '.join(missing)}")
    if unknown:
        raise Refusal(f"{where}: unknown field(s): {', '.join(unknown)}")


def require_hex(value: Any, digits: int, where: str) -> str:
    if not isinstance(value, str) or len(value) != digits:
        raise Refusal(f"{where}: must be exactly {digits} hexadecimal digits")
    try:
        int(value, 16)
    except ValueError as error:
        raise Refusal(f"{where}: must be hexadecimal") from error
    return value.lower()


def validate_identity(value: Any, where: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise Refusal(f"{where}: must be an object")
    required = set(HEX32_FIELDS + DIGEST_FIELDS) | {
        "platform",
        "disc_number",
        "disc_count",
        "savegame_id",
    }
    require_exact_keys(value, required, where)

    identity = dict(value)
    if identity["platform"] != "xenon":
        raise Refusal(f"{where}.platform: only 'xenon' is accepted")
    for field in HEX32_FIELDS:
        identity[field] = require_hex(identity[field], 8, f"{where}.{field}")
    for field in DIGEST_FIELDS:
        identity[field] = require_hex(identity[field], 64, f"{where}.{field}")
    identity["savegame_id"] = require_hex(
        identity["savegame_id"], 8, f"{where}.savegame_id"
    )

    disc_number = identity["disc_number"]
    disc_count = identity["disc_count"]
    if (
        isinstance(disc_number, bool)
        or not isinstance(disc_number, int)
        or isinstance(disc_count, bool)
        or not isinstance(disc_count, int)
        or disc_count < 1
        or disc_number < 1
        or disc_number > disc_count
    ):
        raise Refusal(
            f"{where}: disc_number/disc_count must describe a valid one-based disc"
        )
    return identity


def validate_manifest(path: Path, value: dict[str, Any]) -> tuple[str, dict[str, Any], int, bool]:
    required = {
        "schema_version",
        "game",
        "identity",
        "override_count",
        "has_native_renderer",
    }
    require_exact_keys(value, required, f"manifest {path}")
    if value["schema_version"] != SCHEMA_VERSION:
        raise Refusal(
            f"manifest {path}: schema_version must be {SCHEMA_VERSION}"
        )
    game = value["game"]
    if game not in GAME_NAMES:
        choices = ", ".join(GAME_NAMES)
        raise Refusal(f"manifest {path}.game: expected one of {choices}")

    override_count = value["override_count"]
    if (
        isinstance(override_count, bool)
        or not isinstance(override_count, int)
        or override_count < 0
    ):
        raise Refusal(f"manifest {path}.override_count: must be a non-negative integer")
    if not isinstance(value["has_native_renderer"], bool):
        raise Refusal(f"manifest {path}.has_native_renderer: must be boolean")

    return (
        game,
        validate_identity(value["identity"], f"manifest {path}.identity"),
        override_count,
        value["has_native_renderer"],
    )


def validate_evidence(
    value: Any, gate: str, result_path: Path, game: str
) -> tuple[str, ...]:
    if not isinstance(value, list) or not value:
        raise Refusal(f"results {result_path}.{gate}.evidence: must be a non-empty array")

    summaries = []
    for index, item in enumerate(value):
        where = f"results {result_path}.{gate}.evidence[{index}]"
        if not isinstance(item, dict):
            raise Refusal(f"{where}: must be an object")
        require_exact_keys(item, {"artifact", "sha256", "source", "summary"}, where)
        artifact = item["artifact"]
        source = item["source"]
        summary = item["summary"]
        if not isinstance(artifact, str) or not artifact:
            raise Refusal(f"{where}.artifact: must be a non-empty path")
        if not isinstance(summary, str) or not summary.strip():
            raise Refusal(f"{where}.summary: must be non-empty")
        if not isinstance(source, str) or not source.strip():
            raise Refusal(f"{where}.source: must be non-empty")
        if game != "gears1" and "xenia" in source.strip().lower():
            raise Refusal(
                f"{where}.source: Xenia is not an accepted oracle for {GAME_NAMES[game]}"
            )
        expected = require_hex(item["sha256"], 64, f"{where}.sha256")

        artifact_path = Path(artifact)
        if artifact_path.is_absolute() or ".." in artifact_path.parts:
            raise Refusal(f"{where}.artifact: must stay relative to the results directory")
        artifact_path = (result_path.parent / artifact_path).resolve()
        if not artifact_path.is_relative_to(result_path.parent.resolve()):
            raise Refusal(f"{where}.artifact: escapes the results directory")
        if not artifact_path.is_file():
            raise Refusal(f"{where}.artifact: {artifact_path} is not a file")
        actual = sha256_file(artifact_path)
        if actual != expected:
            raise Refusal(
                f"{where}: digest mismatch for {artifact_path}; expected {expected}, got {actual}"
            )
        summaries.append(f"{source.strip()}: {summary.strip()}")
    return tuple(summaries)


def validate_gate(
    gate: str,
    value: Any,
    result_path: Path,
    game: str,
    override_count: int,
    has_native_renderer: bool,
) -> tuple[str, tuple[str, ...]]:
    if not isinstance(value, dict):
        raise Refusal(f"results {result_path}.{gate}: must be an object")
    status = value.get("status")
    if status not in STATUSES:
        raise Refusal(
            f"results {result_path}.{gate}.status: expected pass, fail, or not_applicable"
        )

    if status == "not_applicable":
        require_exact_keys(value, {"status", "reason"}, f"results {result_path}.{gate}")
        reason = value["reason"]
        if not isinstance(reason, str) or not reason.strip():
            raise Refusal(f"results {result_path}.{gate}.reason: must be non-empty")
        allowed = (gate == "override_ab" and override_count == 0) or (
            gate == "renderer_native_parity" and not has_native_renderer
        )
        if not allowed:
            raise Refusal(
                f"results {result_path}.{gate}: not_applicable contradicts the manifest"
            )
        return status, (reason.strip(),)

    require_exact_keys(value, {"status", "evidence"}, f"results {result_path}.{gate}")
    if gate == "override_ab" and override_count == 0:
        raise Refusal(
            f"results {result_path}.{gate}: manifest declares zero overrides; use not_applicable"
        )
    if gate == "renderer_native_parity" and not has_native_renderer:
        raise Refusal(
            f"results {result_path}.{gate}: manifest declares no native renderer; "
            "use not_applicable"
        )
    return status, validate_evidence(value["evidence"], gate, result_path, game)


def evaluate_case(manifest_path: Path, result_path: Path) -> CaseReport:
    manifest = load_json(manifest_path, "manifest")
    results = load_json(result_path, "results")
    game, manifest_identity, override_count, has_native_renderer = validate_manifest(
        manifest_path, manifest
    )

    require_exact_keys(
        results,
        {"schema_version", "manifest_sha256", "game", "identity", "gates"},
        f"results {result_path}",
    )
    if results["schema_version"] != SCHEMA_VERSION:
        raise Refusal(f"results {result_path}: schema_version must be {SCHEMA_VERSION}")
    if results["game"] != game:
        raise Refusal(
            f"results {result_path}.game: {results['game']!r} does not match manifest {game!r}"
        )
    result_identity = validate_identity(
        results["identity"], f"results {result_path}.identity"
    )
    if result_identity != manifest_identity:
        raise Refusal(f"results {result_path}: exact build identity does not match manifest")

    expected_manifest_digest = require_hex(
        results["manifest_sha256"], 64, f"results {result_path}.manifest_sha256"
    )
    actual_manifest_digest = sha256_file(manifest_path)
    if expected_manifest_digest != actual_manifest_digest:
        raise Refusal(
            f"results {result_path}: manifest digest mismatch; expected "
            f"{expected_manifest_digest}, got {actual_manifest_digest}"
        )

    gates = results["gates"]
    if not isinstance(gates, dict):
        raise Refusal(f"results {result_path}.gates: must be an object")
    require_exact_keys(gates, set(ALL_GATES), f"results {result_path}.gates")

    statuses: dict[str, str] = {}
    summaries: dict[str, tuple[str, ...]] = {}
    for gate in ALL_GATES:
        status, gate_summaries = validate_gate(
            gate, gates[gate], result_path, game, override_count, has_native_renderer
        )
        statuses[gate] = status
        summaries[gate] = gate_summaries

    compatibility_ready = all(statuses[gate] == "pass" for gate in COMPATIBILITY_GATES)
    if not has_native_renderer:
        native_parity = "not_applicable"
    elif statuses["renderer_native_parity"] != "pass":
        native_parity = "not_ready"
    elif statuses["override_ab"] in {"pass", "not_applicable"}:
        native_parity = "ready"
    else:
        native_parity = "not_ready"

    return CaseReport(
        game=game,
        identity=manifest_identity,
        gates=statuses,
        summaries=summaries,
        compatibility_ready=compatibility_ready,
        native_parity=native_parity,
        sixty_fps_ready=statuses["gameplay_60fps"] == "pass",
    )


def print_text(reports: list[CaseReport]) -> None:
    for report_index, report in enumerate(reports):
        if report_index:
            print()
        identity = report.identity
        print(
            f"{GAME_NAMES[report.game]} [{report.game}] exact image "
            f"{identity['image_sha256'][:12]}"
        )
        print(
            f"  title/media {identity['title_id']}/{identity['media_id']}  "
            f"XEX {identity['xex_version']} base {identity['base_version']}  "
            f"disc {identity['disc_number']}/{identity['disc_count']}"
        )
        for gate in ALL_GATES:
            print(f"  {gate:26} {report.gates[gate].upper()}")
            for summary in report.summaries[gate]:
                print(f"    - {summary}")
        compatibility = "READY" if report.compatibility_ready else "NOT READY"
        sixty_fps = "READY" if report.sixty_fps_ready else "NOT READY"
        print(f"  recomp-path compatibility: {compatibility}")
        print(f"  native renderer parity:    {report.native_parity.upper().replace('_', ' ')}")
        print(f"  60 fps enhancement:        {sixty_fps}")


def run_check(inputs: list[str], as_json: bool) -> int:
    if len(inputs) == 0 or len(inputs) % 2 != 0:
        raise Refusal("check requires MANIFEST RESULTS pairs")
    reports = []
    for index in range(0, len(inputs), 2):
        reports.append(evaluate_case(Path(inputs[index]), Path(inputs[index + 1])))

    if as_json:
        print(json.dumps({"schema_version": SCHEMA_VERSION,
                          "cases": [report.to_json() for report in reports]}, indent=2))
    else:
        print_text(reports)
    return 0 if all(report.compatibility_ready for report in reports) else 1


def synthetic_identity(seed: str) -> dict[str, Any]:
    digit = format((ord(seed) % 14) + 1, "x")
    return {
        "platform": "xenon",
        "title_id": digit * 8,
        "media_id": "1" * 8,
        "xex_version": "2" * 8,
        "base_version": "3" * 8,
        "disc_number": 1,
        "disc_count": 1,
        "savegame_id": "4" * 8,
        "xex_sha256": digit * 64,
        "image_sha256": "5" * 64,
    }


def write_synthetic_case(
    root: Path,
    game: str,
    *,
    override_count: int = 1,
    has_native_renderer: bool = True,
) -> tuple[Path, Path, dict[str, Any]]:
    case = root / game
    case.mkdir()
    identity = synthetic_identity(game[-1])
    manifest = {
        "schema_version": SCHEMA_VERSION,
        "game": game,
        "identity": identity,
        "override_count": override_count,
        "has_native_renderer": has_native_renderer,
    }
    manifest_path = case / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    gates: dict[str, Any] = {}
    for gate in ALL_GATES:
        if gate == "override_ab" and override_count == 0:
            gates[gate] = {"status": "not_applicable", "reason": "synthetic zero overrides"}
            continue
        if gate == "renderer_native_parity" and not has_native_renderer:
            gates[gate] = {
                "status": "not_applicable",
                "reason": "synthetic recomp renderer only",
            }
            continue
        artifact = case / f"{gate}.txt"
        artifact.write_text(f"synthetic evidence for {game} {gate}\n", encoding="utf-8")
        gates[gate] = {
            "status": "pass",
            "evidence": [{
                "artifact": artifact.name,
                "sha256": sha256_file(artifact),
                "source": "synthetic_control",
                "summary": f"synthetic {gate} control",
            }],
        }

    results = {
        "schema_version": SCHEMA_VERSION,
        "manifest_sha256": sha256_file(manifest_path),
        "game": game,
        "identity": identity,
        "gates": gates,
    }
    result_path = case / "results.json"
    result_path.write_text(json.dumps(results, indent=2), encoding="utf-8")
    return manifest_path, result_path, results


def expect_refusal(action: Any, phrase: str) -> None:
    try:
        action()
    except Refusal as error:
        if phrase not in str(error):
            raise AssertionError(f"refusal did not contain {phrase!r}: {error}") from error
    else:
        raise AssertionError(f"expected refusal containing {phrase!r}")


def selftest() -> int:
    scratch = Path(__file__).resolve().parents[1] / "scratch"
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="title-conformance-", dir=scratch) as directory:
        root = Path(directory)
        cases = [write_synthetic_case(root, game) for game in GAME_NAMES]
        reports = [evaluate_case(manifest, results) for manifest, results, _ in cases]
        assert [report.game for report in reports] == list(GAME_NAMES)
        assert all(report.compatibility_ready for report in reports)
        assert all(report.native_parity == "ready" for report in reports)
        assert all(report.sixty_fps_ready for report in reports)

        text_output = io.StringIO()
        with redirect_stdout(text_output):
            assert run_check([str(cases[0][0]), str(cases[0][1])], False) == 0
        text_report = text_output.getvalue()
        assert "recomp-path compatibility: READY" in text_report
        assert "native renderer parity:    READY" in text_report
        assert "60 fps enhancement:        READY" in text_report
        assert "supported" not in text_report.lower()

        json_output = io.StringIO()
        with redirect_stdout(json_output):
            assert run_check([str(cases[0][0]), str(cases[0][1])], True) == 0
        json_report = json.loads(json_output.getvalue())
        assert json_report["cases"][0]["game"] == "gears1"
        assert json_report["cases"][0]["compatibility_ready"] is True
        assert json_report["cases"][0]["sixty_fps_ready"] is True

        old_schema = json.loads(cases[0][1].read_text(encoding="utf-8"))
        old_schema["schema_version"] = 1
        old_schema_path = cases[0][1].parent / "old-schema.json"
        old_schema_path.write_text(json.dumps(old_schema), encoding="utf-8")
        expect_refusal(
            lambda: evaluate_case(cases[0][0], old_schema_path),
            f"schema_version must be {SCHEMA_VERSION}",
        )

        no_native_root = root / "no-native"
        no_native_root.mkdir()
        manifest, results, result_data = write_synthetic_case(
            no_native_root, "gears1", override_count=0, has_native_renderer=False
        )
        no_native = evaluate_case(manifest, results)
        assert no_native.compatibility_ready
        assert no_native.native_parity == "not_applicable"

        # A recognised exact identity alone is never compatibility evidence.
        identity_only = json.loads(json.dumps(result_data))
        identity_only["gates"] = {"identity": identity_only["gates"]["identity"]}
        identity_only_path = results.parent / "identity-only.json"
        identity_only_path.write_text(json.dumps(identity_only), encoding="utf-8")
        expect_refusal(
            lambda: evaluate_case(manifest, identity_only_path), "missing field(s)"
        )

        missing_60fps = json.loads(results.read_text(encoding="utf-8"))
        del missing_60fps["gates"]["gameplay_60fps"]
        missing_60fps_path = results.parent / "missing-60fps.json"
        missing_60fps_path.write_text(json.dumps(missing_60fps), encoding="utf-8")
        expect_refusal(
            lambda: evaluate_case(manifest, missing_60fps_path), "gameplay_60fps"
        )

        deferred_60fps = json.loads(results.read_text(encoding="utf-8"))
        deferred_60fps["gates"]["gameplay_60fps"]["status"] = "fail"
        deferred_60fps_path = results.parent / "deferred-60fps.json"
        deferred_60fps_path.write_text(json.dumps(deferred_60fps), encoding="utf-8")
        deferred_report = evaluate_case(manifest, deferred_60fps_path)
        assert deferred_report.compatibility_ready
        assert not deferred_report.sixty_fps_ready
        deferred_output = io.StringIO()
        with redirect_stdout(deferred_output):
            assert run_check([str(manifest), str(deferred_60fps_path)], False) == 0
        assert "recomp-path compatibility: READY" in deferred_output.getvalue()
        assert "60 fps enhancement:        NOT READY" in deferred_output.getvalue()

        absent = json.loads(results.read_text(encoding="utf-8"))
        absent["gates"]["content_mount"]["evidence"] = []
        absent_path = results.parent / "absent-evidence.json"
        absent_path.write_text(json.dumps(absent), encoding="utf-8")
        expect_refusal(
            lambda: evaluate_case(manifest, absent_path), "must be a non-empty array"
        )

        unknown = json.loads(results.read_text(encoding="utf-8"))
        unknown["gates"]["menu"] = {"status": "unknown"}
        unknown_path = results.parent / "unknown.json"
        unknown_path.write_text(json.dumps(unknown), encoding="utf-8")
        expect_refusal(lambda: evaluate_case(manifest, unknown_path), "expected pass")

        mismatched = json.loads(results.read_text(encoding="utf-8"))
        mismatched["identity"]["image_sha256"] = "f" * 64
        mismatch_path = results.parent / "mismatch.json"
        mismatch_path.write_text(json.dumps(mismatched), encoding="utf-8")
        expect_refusal(
            lambda: evaluate_case(manifest, mismatch_path), "identity does not match"
        )

        tampered = json.loads(results.read_text(encoding="utf-8"))
        evidence_path = results.parent / "identity.txt"
        evidence_path.write_text("tampered synthetic evidence\n", encoding="utf-8")
        tampered_path = results.parent / "tampered.json"
        tampered_path.write_text(json.dumps(tampered), encoding="utf-8")
        expect_refusal(lambda: evaluate_case(manifest, tampered_path), "digest mismatch")

        escaped = json.loads(results.read_text(encoding="utf-8"))
        escaped["gates"]["identity"]["evidence"][0]["artifact"] = "../outside.txt"
        escaped_path = results.parent / "escaped.json"
        escaped_path.write_text(json.dumps(escaped), encoding="utf-8")
        expect_refusal(
            lambda: evaluate_case(manifest, escaped_path),
            "must stay relative",
        )

        xenia_only = json.loads(cases[1][1].read_text(encoding="utf-8"))
        xenia_only["gates"]["renderer_compatibility"]["evidence"][0]["source"] = (
            "Xenia Canary reference"
        )
        xenia_only_path = cases[1][1].parent / "xenia-only.json"
        xenia_only_path.write_text(json.dumps(xenia_only), encoding="utf-8")
        expect_refusal(
            lambda: evaluate_case(cases[1][0], xenia_only_path),
            "not an accepted oracle",
        )

        failed = json.loads(cases[1][1].read_text(encoding="utf-8"))
        failed["gates"]["gameplay"]["status"] = "fail"
        failed_path = cases[1][1].parent / "failed.json"
        failed_path.write_text(json.dumps(failed), encoding="utf-8")
        failed_report = evaluate_case(cases[1][0], failed_path)
        assert not failed_report.compatibility_ready
        assert failed_report.native_parity == "ready"
        failed_output = io.StringIO()
        with redirect_stdout(failed_output):
            assert run_check([str(cases[1][0]), str(failed_path)], False) == 1
        assert "recomp-path compatibility: NOT READY" in failed_output.getvalue()

    print(
        "title conformance selftest passed: four-title distinction, exact-build binding, "
        "schema-version, identity-only, missing-60fps, deferred-60fps compatibility, "
        "and unknown-evidence refusal, "
        "artifact tamper/escape detection, "
        "non-Gears-1 Xenia-oracle refusal, CLI reporting/exit status, "
        "compatibility failure, and explicit "
        "no-native/zero-override handling"
    )
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true", help="run synthetic stdlib tests")
    subparsers = parser.add_subparsers(dest="command")
    check = subparsers.add_parser("check", help="evaluate MANIFEST RESULTS pairs")
    check.add_argument("--json", action="store_true", help="emit a machine-readable report")
    check.add_argument("inputs", nargs="+", metavar="JSON")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if args.selftest:
            if args.command is not None:
                raise Refusal("--selftest cannot be combined with a command")
            return selftest()
        if args.command == "check":
            return run_check(args.inputs, args.json)
        raise Refusal("choose 'check' or --selftest")
    except Refusal as error:
        print(f"REFUSED: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Refuse retired CPU-product surfaces anywhere in the first-party tree."""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path


EXCLUDED_DIRECTORIES = {
    ".git",
    ".venv",
    "__pycache__",
    "build",
    "extern",
    "scratch",
}
TEXT_SUFFIXES = {
    ".c",
    ".cc",
    ".cmake",
    ".cpp",
    ".cxx",
    ".h",
    ".hpp",
    ".in",
    ".md",
    ".py",
    ".sh",
    ".toml",
    ".txt",
    ".yaml",
    ".yml",
}
ROOT_TEXT_FILES = {"CMakeLists.txt", "README", "README.md", "AGENTS.md"}

# Join fragments so this checker doesn't exempt itself from the policy it enforces.
OLD_PRODUCT_PATTERNS = (
    re.compile(r"\b" + "xenon" + "recomp" + r"\b", re.IGNORECASE),
    re.compile(r"\bppc_" + "recomp" + r"\b", re.IGNORECASE),
    re.compile(r"\b" + "re" + "compiled" + r"\b", re.IGNORECASE),
    re.compile(r"\b" + "re" + "compiler" + r"\b", re.IGNORECASE),
    re.compile(r"\b" + "re" + "compilation" + r"\b", re.IGNORECASE),
    re.compile(r"\bstatic[- ]" + "recomp" + r"\w*\b", re.IGNORECASE),
    re.compile(r"GEARS_" + "RECOMP" + r"_", re.IGNORECASE),
    re.compile(r"ExecutionKind::" + "Re" + "compiled"),
    re.compile(r"\b" + "Re" + "compiled" + r"[A-Z][A-Za-z0-9_]*"),
    re.compile(r"\bretained (?:body|helper|arm)s?\b", re.IGNORECASE),
    re.compile(r"\bsuper[- ]calls?\b", re.IGNORECASE),
    re.compile(r"\b__imp__" + "sub_" + r"[0-9A-Fa-f]+\b"),
    re.compile(r"\bgenerated (?:guest )?" + "bod" + r"(?:y|ies)\b", re.IGNORECASE),
)
DIRECT_CONSOLE_PATTERNS = (
    re.compile(r"\bfprintf\s*\(\s*stderr\b"),
    re.compile(r"(?<![A-Za-z0-9_])(?:std::)?printf\s*\("),
)
DIRECT_PROCESS_PATTERNS = (
    re.compile(r"(?<![A-Za-z0-9_])(?:std::)?" + "get" + r"env\s*\("),
    re.compile(r"(?<![A-Za-z0-9_])(?:std::)?" + "sys" + r"tem\s*\("),
)
STALE_CONFORMANCE_PATTERNS = (
    re.compile(r"\bgenerated[-_ ]" + "cpu" + r"\b", re.IGNORECASE),
    re.compile(r"\brecomp[-_ ]" + "path compatibility\b", re.IGNORECASE),
)


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    reason: str


@dataclass(frozen=True)
class ScanResult:
    files: int
    bytes: int
    findings: tuple[Finding, ...]


def candidate_files(root: Path) -> list[Path]:
    candidates: list[Path] = []
    for path in root.rglob("*"):
        if any(part in EXCLUDED_DIRECTORIES for part in path.relative_to(root).parts):
            continue
        if not path.is_file():
            continue
        if path.name in ROOT_TEXT_FILES or path.suffix.lower() in TEXT_SUFFIXES:
            candidates.append(path)
    return sorted(candidates)


def scan(root: Path) -> ScanResult:
    findings: list[Finding] = []
    files = 0
    scanned_bytes = 0
    for path in candidate_files(root):
        relative = path.relative_to(root).as_posix()
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as error:
            findings.append(Finding(relative, 0, f"cannot scan first-party text: {error}"))
            continue
        files += 1
        scanned_bytes += len(text.encode("utf-8"))
        for line_number, line in enumerate(text.splitlines(), 1):
            for pattern in OLD_PRODUCT_PATTERNS:
                if pattern.search(line):
                    findings.append(
                        Finding(relative, line_number, "retired CPU-product terminology")
                    )
                    break
            for pattern in STALE_CONFORMANCE_PATTERNS:
                if pattern.search(line):
                    findings.append(
                        Finding(relative, line_number, "stale CPU-product conformance vocabulary")
                    )
                    break
            if path.suffix.lower() in {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"} and (
                relative.startswith("runtime/")
                or relative.startswith("tools/")
                or relative.startswith("xenia_gpu/")
            ):
                for pattern in DIRECT_CONSOLE_PATTERNS:
                    if pattern.search(line):
                        findings.append(
                            Finding(relative, line_number, "direct C/C++ diagnostic output")
                        )
                        break
                for pattern in DIRECT_PROCESS_PATTERNS:
                    if pattern.search(line):
                        findings.append(
                            Finding(relative, line_number, "direct process environment or shell access")
                        )
                        break

    shell_files = [
        path.relative_to(root).as_posix()
        for path in candidate_files(root)
        if path.suffix.lower() == ".sh"
    ]
    for relative in shell_files:
        if relative != "run.sh":
            findings.append(Finding(relative, 0, "run.sh must be the sole shell entry point"))
    return ScanResult(files, scanned_bytes, tuple(findings))


def main(argv: list[str]) -> int:
    root = Path(__file__).resolve().parents[1]
    if argv[1:]:
        print(f"usage: {argv[0]}", file=sys.stderr)
        return 2

    result = scan(root)
    for finding in result.findings:
        location = f":{finding.line}" if finding.line else ""
        print(f"REFUSING: {finding.path}{location}: {finding.reason}", file=sys.stderr)
    if result.findings:
        print(
            f"migration boundary failed: scanned {result.files} files / "
            f"{result.bytes} bytes; found {len(result.findings)} violation(s)",
            file=sys.stderr,
        )
        return 1
    print(
        f"migration boundary passed: scanned {result.files} first-party files / "
        f"{result.bytes} bytes; found 0 violations"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

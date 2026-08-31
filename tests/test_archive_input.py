"""Focused tests for bounded 7z disc-image materialization."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.gearsue3_bootstrap import archive


class FakeRunner:
    def __init__(self, listing: str, image: bytes = b"disc") -> None:
        self.listing = listing
        self.image = image
        self.captured: list[list[str | Path]] = []
        self.runs = 0

    def capture(self, command: list[str | Path], *, cwd: Path) -> str:
        self.captured.append(command)
        return self.listing

    def run(self, command: list[str | Path], *, cwd: Path) -> None:
        self.captured.append(command)
        self.runs += 1
        output_option = next(value for value in command if str(value).startswith("-o"))
        output = Path(str(output_option)[2:])
        output.mkdir(parents=True, exist_ok=True)
        (output / "disc.iso").write_bytes(self.image)


def listing(*members: tuple[str, int, bool, bool]) -> str:
    records = [
        "Path = archive.7z\nType = 7z\n",
        *(
            "\n".join(
                [
                    f"Path = {path}",
                    f"Size = {size}",
                    f"Folder = {'+' if folder else '-'}",
                    f"Encrypted = {'+' if encrypted else '-'}",
                ]
            )
            for path, size, folder, encrypted in members
        ),
    ]
    return "\n\n".join(records)


class ArchiveInputTests(unittest.TestCase):
    def test_listing_selects_one_safe_image_and_ignores_archive_header(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            archive_path = Path(temporary) / "input.7z"
            archive_path.write_bytes(b"archive")
            runner = FakeRunner(
                listing(
                    ("game/readme.txt", 4, False, False),
                    ("nested/game.iso", 8, False, False),
                )
            )
            entries = archive.inspect_archive(
                archive_path, runner=runner, cwd=Path(temporary)
            )
            self.assertEqual(
                [entry.path for entry in entries],
                ["game/readme.txt", "nested/game.iso"],
            )

    def test_duplicate_or_unsafe_images_are_refused(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            archive_path = Path(temporary) / "input.7z"
            archive_path.write_bytes(b"archive")
            duplicate_entries = archive.inspect_archive(
                archive_path,
                runner=FakeRunner(
                    listing(
                        ("one.iso", 1, False, False),
                        ("two.iso", 1, False, False),
                    )
                ),
                cwd=Path(temporary),
            )
            with self.assertRaisesRegex(archive.ArchiveError, "exactly one disc image"):
                archive._image_entry(duplicate_entries)

            with self.assertRaisesRegex(archive.ArchiveError, "unsafe path"):
                archive.inspect_archive(
                    archive_path,
                    runner=FakeRunner(listing(("../disc.iso", 1, False, False))),
                    cwd=Path(temporary),
                )

    def test_materialization_is_content_addressed_and_reused(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive_path = root / "input.7z"
            archive_path.write_bytes(b"archive")
            runner = FakeRunner(listing(("nested/disc.iso", 4, False, False)))
            first = archive.materialize_disc_image(
                archive_path, root / "archives", runner=runner, cwd=root
            )
            second = archive.materialize_disc_image(
                archive_path, root / "archives", runner=runner, cwd=root
            )
            self.assertEqual(first, second)
            self.assertEqual(first.read_bytes(), b"disc")
            self.assertEqual(runner.runs, 1)


if __name__ == "__main__":
    unittest.main()

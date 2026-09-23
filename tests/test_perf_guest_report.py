"""Resolution of perf samples against the product's JIT perf map."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from tools import perf_guest_report as report

MAP = ["a0000000 40 guest_82000000", "a0000100 20 guest_8222F460"]


class PerfGuestReportTest(unittest.TestCase):
    def test_names_samples_inside_functions_and_counts_the_rest_as_host(self) -> None:
        perf_map = report.parse_perf_map(MAP)
        samples = report.parse_samples(
            [
                " Main XThread (F   a0000010",
                " Main XThread (F   a000011f",
                " Main XThread (F   a0000120",  # one past the second function's end
                " GPU Commands (0   5f0000",
            ]
        )
        threads = report.attribute(samples, perf_map)
        self.assertEqual(threads["Main XThread (F"]["guest_82000000"], 1)
        self.assertEqual(threads["Main XThread (F"]["guest_8222F460"], 1)
        self.assertEqual(threads["Main XThread (F"][report.HOST], 1)
        self.assertEqual(threads["GPU Commands (0"][report.HOST], 1)
        text = report.render(threads, thread_count=2, function_count=3)
        self.assertIn("Main XThread (F: 3 samples, 75.0% of 4", text)
        self.assertIn("33.3%  guest_8222F460", text)

    def test_refuses_a_recording_that_never_touches_the_map(self) -> None:
        perf_map = report.parse_perf_map(MAP)
        samples = report.parse_samples([" XThread1 5f0000", " XThread1 b0000000"])
        with self.assertRaisesRegex(ValueError, "no sample of 2 falls in any of the map's 2"):
            report.attribute(samples, perf_map)

    def test_refuses_malformed_or_empty_inputs(self) -> None:
        with self.assertRaisesRegex(ValueError, "line 2"):
            report.parse_perf_map(["a0000000 40 guest_82000000", "not a map line"])
        with self.assertRaisesRegex(ValueError, "names no translated function"):
            report.parse_perf_map([""])
        with self.assertRaisesRegex(ValueError, "sample line 1"):
            report.parse_samples(["thread without address"])
        with self.assertRaisesRegex(ValueError, "no samples"):
            report.parse_samples([])


if __name__ == "__main__":
    unittest.main()

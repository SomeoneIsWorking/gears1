"""The idle-frame parity check against planted differences."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from tools.frame_parity import FrameParityError, compare_idle, mean_difference


def scene(seed: int = 7) -> np.ndarray:
    """A textured 1280x720 frame with far more colours than the oracle floor."""

    rng = np.random.default_rng(seed)
    return rng.integers(0, 256, size=(720, 1280, 3)).astype(np.float32)


def with_motion(frame: np.ndarray, top: int) -> np.ndarray:
    """The same view with a small animated patch, as an idle scene has."""

    moved = frame.copy()
    moved[top : top + 40, 600:680] = 255 - moved[top : top + 40, 600:680]
    return moved


class FrameParityTests(unittest.TestCase):
    def setUp(self) -> None:
        base = scene()
        self.oracle = (with_motion(base, 100), with_motion(base, 300))
        self.product = (with_motion(base, 200), with_motion(base, 400))

    def test_an_equal_rendering_out_of_phase_matches(self) -> None:
        parity = compare_idle(self.oracle, self.product)
        self.assertGreater(parity.difference, 0.0)
        self.assertTrue(parity.matches, parity.report())

    def test_planted_defects_differ(self) -> None:
        def hole(frame: np.ndarray) -> np.ndarray:
            holed = frame.copy()
            holed[300:420, 500:700] = 0
            return holed

        defects = {
            "brighter": lambda frame: np.clip(frame * 1.1, 0, 255),
            "hole": hole,
            "shifted": lambda frame: np.roll(frame, 2, axis=1),
            "swapped": lambda frame: frame[..., ::-1],
        }
        for name, defect in defects.items():
            with self.subTest(defect=name):
                product = (defect(self.product[0]), defect(self.product[1]))
                parity = compare_idle(self.oracle, product)
                self.assertFalse(parity.matches, parity.report())

    def test_a_flickering_product_does_not_widen_its_own_limit(self) -> None:
        flicker = np.clip(self.product[1] * 1.1, 0, 255)
        parity = compare_idle(self.oracle, (self.product[0], flicker))
        self.assertGreater(parity.product_motion, parity.limit)
        self.assertFalse(parity.matches, parity.report())

    def test_a_blank_oracle_view_is_refused(self) -> None:
        blank = np.zeros((720, 1280, 3), dtype=np.float32)
        with self.assertRaisesRegex(FrameParityError, "not a rendered scene"):
            compare_idle((blank, blank), (blank, blank))

    def test_frames_of_different_sizes_are_refused(self) -> None:
        with self.assertRaisesRegex(FrameParityError, "differ in size"):
            mean_difference(scene()[:360], scene())


if __name__ == "__main__":
    unittest.main()

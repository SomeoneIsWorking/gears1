"""Whether the product renders a still scene as stock Xenia does.

Two emulations are never frame-locked, so an exact pixel match is not the
question. Both are brought to the same idle view, and each is measured against
itself between its last two captures: the oracle's self-difference is the
scene's own motion (idle animation, particles, flicker). The product matches
when its difference from the oracle stays within ``MOTION_MULTIPLE`` times that
motion. Only the oracle sets the limit: a product that flickers must not widen
its own tolerance. The product's self-difference is reported beside it.

The multiple was set from measurement on Gears 1's first idle view (cell block,
1280x720). The product-versus-oracle difference was 0.31 against the oracle's
own 0.32. Planted defects in the product frame scored between 1.04 and 7.9,
3.3 to 25 times that motion: a 10% brightness change, a 120x200 black hole, a
two-pixel shift, gamma 0.9, swapped red and blue, and black. Two times the
motion passes the first and fails every one of them.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np
from PIL import Image

MOTION_MULTIPLE = 2.0
# Below this the oracle's view is a blank or loading screen, where two black
# frames would "match" and prove nothing. Its idle gameplay views measure
# 14k-34k colours.
MIN_ORACLE_COLOURS = 1000


class FrameParityError(RuntimeError):
    """The captures cannot be compared at all."""


@dataclass(frozen=True)
class FrameParity:
    oracle_motion: float
    product_motion: float
    difference: float

    @property
    def limit(self) -> float:
        return MOTION_MULTIPLE * self.oracle_motion

    @property
    def matches(self) -> bool:
        return self.difference <= self.limit

    def report(self) -> dict[str, float | bool]:
        return {**asdict(self), "limit": self.limit, "matches": self.matches}


def load_frame(path: Path) -> np.ndarray:
    with Image.open(path) as image:
        return np.asarray(image.convert("RGB"), dtype=np.float32)


def mean_difference(left: np.ndarray, right: np.ndarray) -> float:
    """Mean absolute per-channel difference, in 8-bit levels."""

    if left.shape != right.shape:
        raise FrameParityError(f"frames differ in size: {left.shape} and {right.shape}")
    return float(np.abs(left - right).mean())


def distinct_colours(frame: np.ndarray) -> int:
    packed = frame.astype(np.uint32)
    return int(np.unique(packed[..., 0] << 16 | packed[..., 1] << 8 | packed[..., 2]).size)


def compare_idle(
    oracle: tuple[np.ndarray, np.ndarray], product: tuple[np.ndarray, np.ndarray]
) -> FrameParity:
    """Each side's last two captures of the same idle view, earlier first."""

    colours = distinct_colours(oracle[1])
    if colours < MIN_ORACLE_COLOURS:
        raise FrameParityError(
            f"the oracle's last frame has {colours} colours (< {MIN_ORACLE_COLOURS}); it is not "
            "a rendered scene, so a match would prove nothing"
        )
    return FrameParity(
        oracle_motion=mean_difference(*oracle),
        product_motion=mean_difference(*product),
        difference=mean_difference(oracle[1], product[1]),
    )

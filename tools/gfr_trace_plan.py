#!/usr/bin/env python3
"""Select capture draws for ordinary and intermediate-copy Xenia traces.

The converter owns binary encoding; this module owns which recorded draw items
are emitted. Keeping selection separate makes an intermediate checkpoint an
explicit trace plan rather than another conditional in the encoder loop.
"""

from dataclasses import dataclass


@dataclass(frozen=True)
class CheckpointPlan:
    """A capture prefix followed by one recorded resolve draw."""

    resolve_index: int
    prefix_count: int
    resolve_draw: int
    depth: bool

    @property
    def draw_indices(self):
        return (*range(self.prefix_count), self.resolve_draw)

    @property
    def present(self):
        # A depth destination cannot be presented as a texture. A normal frame
        # swap is still emitted so the trace closes its frame and flushes all
        # diagnostic readbacks; the checkpoint evidence is the raw copy bytes.
        return "frame" if self.depth else f"resolve:{self.resolve_index}"


def parse_checkpoint(text, resolves, draw_count):
    """Parse RESOLVE:PREFIX and validate that it is a true intermediate copy.

    PREFIX is the number of capture draw items to execute before appending the
    selected capture resolve. It may end immediately before that resolve, but
    never after it: replaying later work and calling the result an earlier
    checkpoint would invert the execution order being measured.
    """
    try:
        resolve_text, prefix_text = text.split(":", 1)
        resolve_index = int(resolve_text, 0)
        prefix_count = int(prefix_text, 0)
    except (AttributeError, TypeError, ValueError):
        raise ValueError("checkpoint copy must be RESOLVE:PREFIX")
    if not 0 <= resolve_index < len(resolves):
        raise ValueError(
            f"resolve {resolve_index} is absent; capture has {len(resolves)} resolve(s)")
    selected = resolves[resolve_index]
    resolve_draw = selected["draw"]
    if not 0 <= prefix_count <= resolve_draw:
        raise ValueError(
            f"prefix {prefix_count} must be between 0 and the selected resolve's "
            f"draw index {resolve_draw}")
    if resolve_draw >= draw_count:
        raise ValueError(
            f"resolve draw {resolve_draw} is outside the capture's {draw_count} draws")
    return CheckpointPlan(
        resolve_index, prefix_count, resolve_draw, selected["depth"])


REGISTER_RUN_GAP = 6


def changed_runs(previous, current, lo, hi, gap=REGISTER_RUN_GAP):
    """Return merged changed-register runs and the raw changed-register count."""
    if previous is None:
        return [(lo, hi)], hi - lo
    indices = [index for index in range(lo, hi)
               if previous[index] != current[index]]
    if not indices:
        return [], 0
    runs = []
    start = last = indices[0]
    for index in indices[1:]:
        if index - last > gap:
            runs.append((start, last + 1))
            start = index
        last = index
    runs.append((start, last + 1))
    return runs, len(indices)


def selftest_cases():
    """Positive and negative cases consumed by gfr_to_xtr's verifier."""
    a = [0] * 20
    b = list(a)
    b[3] = b[11] = 1
    c = list(a)
    c[3] = c[6] = 1
    resolves = [
        {"draw": 8, "depth": False},
        {"draw": 9, "depth": True},
    ]
    checkpoint = parse_checkpoint("0:5", resolves, 10)

    def refuses(specification):
        try:
            parse_checkpoint(specification, resolves, 10)
        except ValueError:
            return True
        return False

    return [
        ("no prior state sends the whole span",
         changed_runs(None, a, 0, 20), ([(0, 20)], 20)),
        ("an identical snapshot sends nothing",
         changed_runs([7] * 20, [7] * 20, 0, 20), ([], 0)),
        ("far-apart changes stay separate runs",
         changed_runs(a, b, 0, 20), ([(3, 4), (11, 12)], 2)),
        ("near changes merge into one run",
         changed_runs(a, c, 0, 20), ([(3, 7)], 2)),
        ("changes outside the span are not reported",
         changed_runs(a, b, 12, 20), ([], 0)),
        ("checkpoint emits the prefix then its real resolve",
         checkpoint.draw_indices, (0, 1, 2, 3, 4, 8)),
        ("checkpoint presents the selected resolve",
         checkpoint.present, "resolve:0"),
        ("depth checkpoint uses a non-depth presentation",
         parse_checkpoint("1:5", resolves, 10).present, "frame"),
        ("checkpoint refuses work after its resolve", refuses("0:9"), True),
    ]

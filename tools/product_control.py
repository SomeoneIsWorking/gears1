#!/usr/bin/env python3
"""Drive a running offscreen product through its loopback control channel.

Start the run with a control port, then press, look, and read status while it
plays. A walk owns the pad until its last step; the channel refuses input
(409) until then.

    uv run --locked python tools/run_offscreen.py --walk gameplay --seconds 900 \\
        --control-port 32125
    uv run --locked python tools/product_control.py --port 32125 pad --ly 32767 --hold 2
    uv run --locked python tools/product_control.py --port 32125 frame scratch/control/now.png
    uv run --locked python tools/product_control.py --port 32125 memory 82BED138 64
"""

from __future__ import annotations

import argparse
import io
import json
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Sequence
from pathlib import Path

from PIL import Image

REQUEST_TIMEOUT_SECONDS = 30
PAD_FIELDS = ("buttons", "lx", "ly", "rx", "ry", "lt", "rt")


class ControlError(RuntimeError):
    """The channel refused a request or could not be reached."""


class ProductControl:
    """The HTTP routes of one product's control channel."""

    def __init__(self, port: int) -> None:
        self._base = f"http://127.0.0.1:{port}"

    def status(self) -> dict[str, object]:
        return json.loads(self._request("GET", "/api/status"))

    def set_pad(self, fields: dict[str, str]) -> None:
        self._request("POST", "/api/input", urllib.parse.urlencode(fields).encode())

    def release(self) -> None:
        self._request("POST", "/api/input/release", b"")

    def frame(self) -> Image.Image:
        return Image.open(io.BytesIO(self._request("GET", "/api/frame.ppm")))

    def player(self) -> dict[str, object]:
        """The local player's view yaws and pawn; ControlError before gameplay."""
        return json.loads(self._request("GET", "/api/player"))

    def memory(self, address: int, length: int) -> bytes:
        query = urllib.parse.urlencode({"address": f"0x{address:08X}", "length": length})
        return self._request("GET", f"/api/memory?{query}")

    def _request(self, method: str, path: str, body: bytes | None = None) -> bytes:
        request = urllib.request.Request(self._base + path, data=body, method=method)
        try:
            with urllib.request.urlopen(request, timeout=REQUEST_TIMEOUT_SECONDS) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            raise ControlError(
                f"{method} {path}: HTTP {error.code}: {error.read().decode(errors='replace')}"
            ) from error
        except urllib.error.URLError as error:
            raise ControlError(f"{method} {path}: {error.reason}") from error


def pad_fields(arguments: argparse.Namespace) -> dict[str, str]:
    """The pad fields given on the command line; omitted ones are neutral."""
    return {
        name: str(getattr(arguments, name))
        for name in PAD_FIELDS
        if getattr(arguments, name) is not None
    }


def hex_dump(address: int, data: bytes) -> str:
    """Sixteen bytes a line, with each big-endian word also read as a float."""
    lines = []
    for offset in range(0, len(data), 16):
        row = data[offset : offset + 16]
        words = " ".join(row[i : i + 4].hex() for i in range(0, len(row), 4))
        floats = " ".join(
            f"{struct.unpack('>f', row[i : i + 4])[0]:.6g}" for i in range(0, len(row) - 3, 4)
        )
        lines.append(f"{address + offset:08X}  {words:<35}  {floats}")
    return "\n".join(lines)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, required=True)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status", help="print the run's presents and pad")
    pad = commands.add_parser("pad", help="replace the whole pad state")
    pad.add_argument("--buttons", help="comma-separated names, e.g. A,START")
    for stick in ("lx", "ly", "rx", "ry"):
        pad.add_argument(f"--{stick}", type=int)
    for trigger in ("lt", "rt"):
        pad.add_argument(f"--{trigger}", type=int)
    pad.add_argument("--hold", type=float, help="release after this many seconds")
    commands.add_parser("release", help="a neutral pad, still connected")
    commands.add_parser("player", help="print the local player's position, view and weapon")
    memory = commands.add_parser("memory", help="dump guest memory as words and floats")
    memory.add_argument("address", type=lambda text: int(text, 16))
    memory.add_argument("length", type=int)
    frame = commands.add_parser("frame", help="save the latest guest output")
    frame.add_argument("output", type=Path)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    control = ProductControl(arguments.port)
    try:
        if arguments.command == "status":
            print(json.dumps(control.status(), indent=2))
        elif arguments.command == "pad":
            control.set_pad(pad_fields(arguments))
            if arguments.hold is not None:
                time.sleep(arguments.hold)
                control.release()
        elif arguments.command == "release":
            control.release()
        elif arguments.command == "player":
            print(json.dumps(control.player(), indent=2))
        elif arguments.command == "memory":
            print(hex_dump(arguments.address, control.memory(arguments.address, arguments.length)))
        else:
            arguments.output.parent.mkdir(parents=True, exist_ok=True)
            control.frame().save(arguments.output)
            print(arguments.output)
    except ControlError as error:
        print(f"product_control: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

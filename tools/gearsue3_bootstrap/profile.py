"""Title-profile facts and deterministic navigation schedule rendering."""

from __future__ import annotations

import re
import tomllib
from dataclasses import dataclass
from pathlib import Path

_HEX32 = re.compile(r"[0-9a-f]{8}")
# The buttons runtime/input.cpp PadButtonByName accepts, the triggers, and stick deflections.
_STEP_INPUT = (r"(?:UP|DOWN|LEFT|RIGHT|START|BACK|[LR]THUMB|[LR]B|[ABXY]|[LR]T"
               r"|L[XY][+-]?|R[XY][+-]?)")
# "ms:" releases everything; "ms:LY+&RX-" holds a chord, as the runtime parses it.
_TIMED_STEP = re.compile(rf"(?:0|[1-9][0-9]*):(?:{_STEP_INPUT}(?:&{_STEP_INPUT})*)?")


class ProfileError(RuntimeError):
    """A tracked title profile is incomplete, malformed, or ambiguous."""


@dataclass(frozen=True)
class TitleIdentity:
    title_id: str
    savegame_id: str
    platform: int
    disc_number: int
    disc_count: int
    xex_sha256: str
    image_sha256: str


@dataclass(frozen=True)
class Navigation:
    menu_walk: str
    menu_walk_min_seconds: int
    start_walk: str
    checkpoint_walk: str
    gameplay_walk: str


@dataclass(frozen=True)
class TitleProfile:
    key: str
    display_name: str
    save_namespace: str
    identity: TitleIdentity
    navigation: Navigation


def _mapping(value: object, description: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise ProfileError(f"{description} must be a table")
    return value


def _string(table: dict[str, object], key: str, description: str) -> str:
    value = table.get(key)
    if not isinstance(value, str) or not value:
        raise ProfileError(f"{description}.{key} must be a non-empty string")
    return value


def _positive_integer(table: dict[str, object], key: str, description: str) -> int:
    value = table.get(key)
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise ProfileError(f"{description}.{key} must be a positive integer")
    return value


def _unsigned_byte(table: dict[str, object], key: str, description: str) -> int:
    value = table.get(key)
    if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value <= 0xFF:
        raise ProfileError(f"{description}.{key} must be an unsigned byte")
    return value


def _sha256(table: dict[str, object], key: str) -> str:
    value = _string(table, key, "identity")
    if len(value) != 64 or any(character not in "0123456789abcdef" for character in value):
        raise ProfileError(f"identity.{key} must be 64 lowercase hexadecimal digits")
    return value


def _validate_timed_walk(schedule: str, description: str) -> None:
    steps = schedule.split(",")
    if not steps or any(_TIMED_STEP.fullmatch(step) is None for step in steps):
        raise ProfileError(f"{description} contains an invalid timed input step")
    times = [int(step.partition(":")[0]) for step in steps]
    if times != sorted(times) or len(times) != len(set(times)):
        raise ProfileError(f"{description} times must be unique and ordered")


def load_profile(repo_root: Path, key: str = "gears1") -> TitleProfile:
    profile_path = repo_root / "config" / "titles" / f"{key}.toml"
    try:
        document = tomllib.loads(profile_path.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise ProfileError(f"cannot read title profile {profile_path}: {error}") from error
    if document.get("schema") != 1:
        raise ProfileError(f"{profile_path}: schema must be 1")
    if _string(document, "key", "profile") != key:
        raise ProfileError(f"{profile_path}: key does not match filename {key!r}")

    identity_table = _mapping(document.get("identity"), "identity")
    title_id = _string(identity_table, "title_id", "identity")
    savegame_id = _string(identity_table, "savegame_id", "identity")
    if _HEX32.fullmatch(title_id) is None or _HEX32.fullmatch(savegame_id) is None:
        raise ProfileError("identity title_id and savegame_id must be 8 lowercase hex digits")

    navigation_table = _mapping(document.get("navigation"), "navigation")
    menu_walk = _string(navigation_table, "menu_walk", "navigation")
    start_walk = _string(navigation_table, "start_walk", "navigation")
    checkpoint_walk = _string(navigation_table, "checkpoint_walk", "navigation")
    gameplay_walk = _string(navigation_table, "gameplay_walk", "navigation")
    _validate_timed_walk(menu_walk, "navigation.menu_walk")
    _validate_timed_walk(start_walk, "navigation.start_walk")
    _validate_timed_walk(checkpoint_walk, "navigation.checkpoint_walk")
    _validate_timed_walk(gameplay_walk, "navigation.gameplay_walk")

    return TitleProfile(
        key=key,
        display_name=_string(document, "display_name", "profile"),
        save_namespace=_string(document, "save_namespace", "profile"),
        identity=TitleIdentity(
            title_id=title_id,
            savegame_id=savegame_id,
            platform=_unsigned_byte(identity_table, "platform", "identity"),
            disc_number=_positive_integer(identity_table, "disc_number", "identity"),
            disc_count=_positive_integer(identity_table, "disc_count", "identity"),
            xex_sha256=_sha256(identity_table, "xex_sha256"),
            image_sha256=_sha256(identity_table, "image_sha256"),
        ),
        navigation=Navigation(
            menu_walk=menu_walk,
            menu_walk_min_seconds=_positive_integer(
                navigation_table, "menu_walk_min_seconds", "navigation"
            ),
            start_walk=start_walk,
            checkpoint_walk=checkpoint_walk,
            gameplay_walk=gameplay_walk,
        ),
    )


_ORACLE_BUTTONS = frozenset({"A", "B", "X", "Y", "START"})


def oracle_timed_schedule(schedule: str) -> str:
    """A timed button walk in the Xenia oracle's grammar: each press at its second.

    The product's steps are ``MS:BUTTON`` presses and ``MS:`` releases; the oracle
    holds each press briefly on its own, so releases carry nothing. A step the
    oracle cannot express the same way (a stick, a trigger, a chord) is refused
    rather than dropped, since a dropped step sends the two runs down different
    routes.
    """

    _validate_timed_walk(schedule, "the oracle walk")
    presses: list[str] = []
    for step in schedule.split(","):
        milliseconds, _, button = step.partition(":")
        if not button:
            continue
        if button not in _ORACLE_BUTTONS:
            raise ProfileError(f"the oracle cannot replay step {step!r}: only single buttons")
        presses.append(f"{button}@{int(milliseconds) / 1000:g}")
    if not presses:
        raise ProfileError("the oracle walk presses nothing")
    return ",".join(presses)

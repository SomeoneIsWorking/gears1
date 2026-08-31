"""Title-profile facts and deterministic navigation schedule rendering."""

from __future__ import annotations

import re
import tomllib
from dataclasses import dataclass
from pathlib import Path

_HEX32 = re.compile(r"[0-9a-f]{8}")
_TIMED_STEP = re.compile(r"(?:0|[1-9][0-9]*):(?:START|[ABXY]|L[XY][+-]?|R[XY][+-]?)?")
_FRAME_ACTION = re.compile(r"(?:START|[ABXY])(?:~[1-9][0-9]*)?|[LR][XY](?:[+-]|0)")


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
    repro_rate_walk: str
    camera_pair_frame_walk: str
    oracle_compare_input: str
    oracle_compare_repeat_start_ms: int
    oracle_compare_repeat_period_ms: int
    oracle_compare_repeat_hold_ms: int
    frame_walk: str
    button_hold_frames: int


@dataclass(frozen=True)
class TitleProfile:
    key: str
    display_name: str
    save_namespace: str
    identity: TitleIdentity
    recompiler_template: Path
    switch_tables_extra: Path
    navigation: Navigation


@dataclass(frozen=True)
class FrameEvent:
    frame: int
    action: str
    duration: int | None


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


def _portable_relative_path(value: str, description: str) -> Path:
    path = Path(value)
    if path.is_absolute() or ".." in path.parts:
        raise ProfileError(f"{description} must be a repository-relative path")
    return path


def _sha256(table: dict[str, object], key: str) -> str:
    value = _string(table, key, "identity")
    if len(value) != 64 or any(character not in "0123456789abcdef" for character in value):
        raise ProfileError(f"identity.{key} must be 64 lowercase hexadecimal digits")
    return value


def _validate_menu_walk(schedule: str) -> None:
    steps = schedule.split(",")
    if not steps or any(_TIMED_STEP.fullmatch(step) is None for step in steps):
        raise ProfileError("navigation.menu_walk contains an invalid timed input step")
    times = [int(step.partition(":")[0]) for step in steps]
    if times != sorted(times) or len(times) != len(set(times)):
        raise ProfileError("navigation.menu_walk times must be unique and ordered")


def parse_frame_walk(schedule: str) -> tuple[FrameEvent, ...]:
    events: list[FrameEvent] = []
    for token in schedule.split():
        frame_text, separator, raw_action = token.partition(":")
        if separator == "" or not frame_text.isdecimal() or int(frame_text) <= 0:
            raise ProfileError(f"invalid frame-walk event {token!r}")
        if _FRAME_ACTION.fullmatch(raw_action) is None:
            raise ProfileError(f"invalid frame-walk action {raw_action!r}")
        action, marker, duration_text = raw_action.partition("~")
        duration = int(duration_text) if marker else None
        events.append(FrameEvent(int(frame_text), action, duration))
    if not events:
        raise ProfileError("navigation.frame_walk must contain at least one event")
    frames = [event.frame for event in events]
    if frames != sorted(frames) or len(frames) != len(set(frames)):
        raise ProfileError("navigation.frame_walk frames must be unique and ordered")
    return tuple(events)


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

    recompiler = _mapping(document.get("recompiler"), "recompiler")
    navigation_table = _mapping(document.get("navigation"), "navigation")
    menu_walk = _string(navigation_table, "menu_walk", "navigation")
    start_walk = _string(navigation_table, "start_walk", "navigation")
    checkpoint_walk = _string(navigation_table, "checkpoint_walk", "navigation")
    repro_rate_walk = _string(navigation_table, "repro_rate_walk", "navigation")
    camera_pair_frame_walk = _string(
        navigation_table, "camera_pair_frame_walk", "navigation"
    )
    frame_walk = _string(navigation_table, "frame_walk", "navigation")
    _validate_menu_walk(menu_walk)
    _validate_menu_walk(start_walk)
    _validate_menu_walk(checkpoint_walk)
    _validate_menu_walk(repro_rate_walk)
    parse_frame_walk(camera_pair_frame_walk)
    parse_frame_walk(frame_walk)

    return TitleProfile(
        key=key,
        display_name=_string(document, "display_name", "profile"),
        save_namespace=_string(document, "save_namespace", "profile"),
        identity=TitleIdentity(
            title_id=title_id,
            savegame_id=savegame_id,
            platform=_positive_integer(identity_table, "platform", "identity"),
            disc_number=_positive_integer(identity_table, "disc_number", "identity"),
            disc_count=_positive_integer(identity_table, "disc_count", "identity"),
            xex_sha256=_sha256(identity_table, "xex_sha256"),
            image_sha256=_sha256(identity_table, "image_sha256"),
        ),
        recompiler_template=_portable_relative_path(
            _string(recompiler, "template", "recompiler"), "recompiler.template"
        ),
        switch_tables_extra=_portable_relative_path(
            _string(recompiler, "switch_tables_extra", "recompiler"),
            "recompiler.switch_tables_extra",
        ),
        navigation=Navigation(
            menu_walk=menu_walk,
            menu_walk_min_seconds=_positive_integer(
                navigation_table, "menu_walk_min_seconds", "navigation"
            ),
            start_walk=start_walk,
            checkpoint_walk=checkpoint_walk,
            repro_rate_walk=repro_rate_walk,
            camera_pair_frame_walk=camera_pair_frame_walk,
            oracle_compare_input=_string(
                navigation_table, "oracle_compare_input", "navigation"
            ),
            oracle_compare_repeat_start_ms=_positive_integer(
                navigation_table, "oracle_compare_repeat_start_ms", "navigation"
            ),
            oracle_compare_repeat_period_ms=_positive_integer(
                navigation_table, "oracle_compare_repeat_period_ms", "navigation"
            ),
            oracle_compare_repeat_hold_ms=_positive_integer(
                navigation_table, "oracle_compare_repeat_hold_ms", "navigation"
            ),
            frame_walk=frame_walk,
            button_hold_frames=_positive_integer(
                navigation_table, "button_hold_frames", "navigation"
            ),
        ),
    )


def native_schedule(navigation: Navigation, schedule: str | None = None) -> str:
    hold = navigation.button_hold_frames
    rendered: list[str] = []
    for event in parse_frame_walk(schedule or navigation.frame_walk):
        if event.action.endswith("0"):
            rendered.append(f"f{event.frame}:")
        elif event.action[-1:] in {"+", "-"}:
            rendered.append(f"f{event.frame}:{event.action}")
        else:
            duration = event.duration or hold
            rendered.extend((f"f{event.frame}:{event.action}", f"f{event.frame + duration}:"))
    return ",".join(rendered)


def oracle_schedule(navigation: Navigation, schedule: str | None = None) -> str:
    hold = navigation.button_hold_frames
    rendered: list[str] = []
    for event in parse_frame_walk(schedule or navigation.frame_walk):
        if event.action.endswith("0") or event.action[-1:] in {"+", "-"}:
            rendered.append(f"{event.action}@{event.frame}")
            continue
        duration = event.duration or hold
        last_start = max(event.frame, event.frame + duration - hold)
        start = event.frame
        while True:
            rendered.append(f"{event.action}@{start}")
            if start >= last_start:
                break
            start = min(start + hold - 1, last_start)
    return ",".join(rendered)


def last_frame(navigation: Navigation, schedule: str | None = None) -> int:
    return max(
        event.frame + (event.duration or 0)
        for event in parse_frame_walk(schedule or navigation.frame_walk)
    )


def native_oracle_compare_schedule(navigation: Navigation, duration_seconds: int) -> str:
    steps = [navigation.start_walk]
    duration_ms = duration_seconds * 1000
    timestamp = navigation.oracle_compare_repeat_start_ms
    while timestamp < duration_ms:
        steps.append(f"{timestamp}:A")
        steps.append(f"{timestamp + navigation.oracle_compare_repeat_hold_ms}:")
        timestamp += navigation.oracle_compare_repeat_period_ms
    return ",".join(steps)

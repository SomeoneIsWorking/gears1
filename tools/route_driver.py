"""Closed-loop play of Gears 1 through the product's control channel.

Route reads the local player (``/api/player``) and the level's navigation
graph (``/api/navigation``) and drives the pad until each step reaches its
goal: walking and mantling along the graph, taking cover, aiming and firing,
reloading or swapping a dry weapon, reviving a downed mate, and reloading the
checkpoint after a death. Each step is recorded, and a step that does not reach
its goal fails, naming it. ``tools/combat_route.py`` composes these steps into
Act 1's route; the pad, clock, and sleep are injected so tests drive a
simulated player.
"""

from __future__ import annotations

import math
import time
from collections.abc import Callable, Collection
from typing import Protocol

from tools.combat_world import (AIM_HEIGHT, AIM_TOLERANCE, NAV_HEIGHT, STICK_LIMIT,
                                TRAVEL_PATH_KINDS, YAW_UNITS_PER_TURN, Pawn, Player, aim_error,
                                aim_stick, choose_cover, distance, stick_toward)
from tools.navigation import NavigationError, NavigationGraph, Point
from tools.product_control import ControlError

POLL_SECONDS = 0.2
# A walk that came less than this closer to its goal in STALL_SECONDS is
# blocked, also when the player moved: in cover the stick slides him along it.
STALL_DISTANCE = 25.0
STALL_SECONDS = 2.5
# Game time must keep to wall time while the product presents up to 120 times
# a second; the bound covers the control channel's sampling latency.
GAME_TIME_RATE_TOLERANCE = 0.03
# A late prompt can need a second hold; a third stall is a real obstacle.
MAX_UNBLOCKS = 2
AIM_SECONDS = 2.0
BURST_SECONDS = 0.5
# Back in cover after a burst, long enough for hits to register.
COVER_SECONDS = 1.2
# Below this health the player stays in cover until it regenerates; one
# exposed burst has cost the player half of its 301 (docs/re-frontier.md).
RECOVER_HEALTH = 280
# Bursts at one target that leave its health unchanged before trying another.
BURSTS_PER_TARGET = 4
# A burst that fires nothing is followed by a reload (RB); a weapon that fires
# nothing after its reload is dry, and the d-pad slots are tried in this order
# for one that is not. Each takes this long before the next burst.
RELOAD_SECONDS = 2.5
SWITCH_SECONDS = 1.0
WEAPON_SLOTS = ("RIGHT", "UP", "LEFT", "DOWN")
AIM_POLL_SECONDS = 0.04
RESPAWN_POLL_SECONDS = 3.0
RESPAWN_SETTLE_SECONDS = 3.0
# A navigation point counts as reached this close, as the engine's own walkers
# accept; the points are about 150 to 200 units apart.
NAV_POINT_RADIUS = 60.0
NAV_POINT_SECONDS = 15.0
# Buttons held, each for seconds, with the stick toward the far side: the first
# A takes cover against the low wall, the second vaults it. Measured live at
# the yard's first cover (-1281, 3640), crossing to (-1284, 3792).
MANTLE_SEQUENCE = (("A", 0.3), (None, 0.8), ("A", 0.3), (None, 1.5))
# Pulling the stick back this long leaves cover: a walk along a cover wall
# stops at its end, as it did from the yard cover.
LEAVE_COVER_SECONDS = 0.8
# What a burst that fired nothing tries next, in order: a tutorial prompt
# (REVIVE) blocks the player's stick and trigger, though not the world's
# clock, until A dismisses it; then RB reloads an empty magazine. A weapon
# that still fires nothing is dry.
MISFIRE_REMEDIES = ("A", "RB")
PROMPT_DISMISS_SECONDS = 0.5
# A downed squad mate lies with health exactly 0 (a dead player reads below 0);
# X revives him from within REVIVE_RADIUS.
REVIVE_RADIUS = 90.0
REVIVE_PRESSES = 5
REVIVE_PRESS_SECONDS = 1.5
# Past the first firefight the route fights from the cover slot choose_cover
# picks (tools/combat_world.py). Marcus is at a slot within COVER_RADIUS of it.
# A slid him into cover up to 300 units on in the view's direction, where a
# drone beside the cover killed him, so he turns along the slot's facing before
# pressing A.
COVER_RADIUS = 80.0
COVER_FACING_DISTANCE = 300.0
COVER_SETTLE_SECONDS = 1.0
# A hostile this close is fired at without a break for CLOSE_BURST_SECONDS,
# the aim following it, however hurt the player is: past the door breach a
# drone charged from 684 to 148 units in 3 s and took 112 health in one hit at
# 295, and both running for other cover from it and waiting in cover for
# health to return cost Marcus his life.
CLOSE_RANGE = 600.0
CLOSE_BURST_SECONDS = 2.0


class RouteFailure(RuntimeError):
    """A step did not reach its goal."""


class Pad(Protocol):
    """The part of the control channel the route drives."""

    def player(self) -> dict[str, object]: ...

    def navigation(self) -> dict[str, object]: ...

    def set_pad(self, fields: dict[str, str]) -> None: ...

    def release(self) -> None: ...


class Route:
    """Runs steps against a pad, recording where each began and ended."""

    def __init__(self, pad: Pad, clock: Callable[[], float] = time.monotonic,
                 sleep: Callable[[float], None] = time.sleep) -> None:
        self._pad = pad
        self._clock = clock
        self._sleep = sleep
        self.steps: list[dict[str, object]] = []
        # Bursts each weapon has fired nothing in since it last fired, and
        # weapons that fired nothing after every remedy. A checkpoint load
        # restores both.
        self._misfires: dict[int, int] = {}
        self._dry: set[int] = set()
        # Game time is compared with wall time only while the level runs: a
        # checkpoint load restarts the world's clock, so it closes the span
        # under way. Wall and game seconds of the closed spans, the span under
        # way, and whether timing is on.
        self._timed_wall = 0.0
        self._timed_world = 0.0
        self._span: tuple[float, float] | None = None
        self._timing = False

    def player(self) -> Player:
        return Player.from_json(self._pad.player())

    def clock(self) -> tuple[float, float]:
        """Wall seconds and the world's game seconds, read together."""

        return self._clock(), self.player().world_seconds

    def start_timing(self) -> None:
        """Start comparing the world's game time with wall time."""

        self._timing = True
        self._open_span()

    def require_real_time(self) -> float:
        """Game seconds per wall second while the world ran; refuses a simulation off real time."""

        if not self._timing:
            raise RouteFailure("game time was never timed")
        self._close_span()
        self._open_span()
        rate = self._timed_world / self._timed_wall
        self.steps.append({"step": "game time kept to wall time", "rate": round(rate, 4),
                           "wall_seconds": round(self._timed_wall, 1)})
        if abs(rate - 1.0) > GAME_TIME_RATE_TOLERANCE:
            raise RouteFailure(f"game time ran at {rate:.3f} game seconds per wall second")
        return rate

    def _open_span(self) -> None:
        if self._timing:
            self._span = self.clock()

    def _close_span(self) -> None:
        if self._span is None:
            return
        wall, world = self.clock()
        self._timed_wall += wall - self._span[0]
        self._timed_world += world - self._span[1]
        self._span = None

    def alive(self, step: str) -> Player:
        player = self.player()
        if player.position is None:
            raise RouteFailure(f"{step}: the player died")
        return player

    def _record(self, step: str, begin: Player, **details: object) -> None:
        end = self.player()
        self.steps.append(
            {"step": step, "from": begin.position, "to": end.position, **details}
        )

    def hold(self, step: str, fields: dict[str, str], seconds: float) -> None:
        begin = self.alive(step)
        self._pad.set_pad(fields)
        self._sleep(seconds)
        self._pad.release()
        self._record(step, begin)

    def walk(self, step: str, goal: tuple[float, float], radius: float, timeout: float,
             unblock: Callable[[], None] | None = None, max_unblocks: int = MAX_UNBLOCKS,
             until: Callable[[Player], bool] | None = None) -> None:
        """Walk until within radius of goal, or until until holds for the player.

        A stall, STALL_SECONDS without coming STALL_DISTANCE closer, runs
        unblock, when given, at most max_unblocks times: the tutorials hold the
        player in place until their button is held, and they trigger where the
        player walks, not where a step begins; low cover the walk runs into
        holds him, sliding along it. Refuses when the
        player dies, stays stalled, or runs out of time.
        """

        begin = self.alive(step)
        start = self._clock()
        mark, mark_time = begin.position, start
        unblocks = 0
        try:
            while True:
                player = self.alive(step)
                if distance(player.position, goal) <= radius or (until and until(player)):
                    break
                now = self._clock()
                if now - start > timeout:
                    raise RouteFailure(
                        f"{step}: still {distance(player.position, goal):.0f} from "
                        f"{goal} after {timeout:.0f} s"
                    )
                if now - mark_time >= STALL_SECONDS:
                    if distance(mark, goal) - distance(player.position, goal) < STALL_DISTANCE:
                        if unblock is None or unblocks == max_unblocks:
                            raise RouteFailure(f"{step}: blocked at {player.position}")
                        self._pad.release()
                        unblock()
                        unblocks += 1
                    mark, mark_time = self.alive(step).position, self._clock()
                    continue
                lx, ly = stick_toward(player.position, player.control_yaw, goal)
                self._pad.set_pad({"lx": str(lx), "ly": str(ly)})
                self._sleep(POLL_SECONDS)
        finally:
            self._pad.release()
        self._record(step, begin, goal=goal, unblocks=unblocks,
                     seconds=round(self._clock() - start, 1))

    def travel(self, step: str, goal: tuple[float, float, float], radius: float,
               unblock: Callable[[], None] | None = None,
               until: Callable[[Player], bool] | None = None) -> None:
        """Walk to goal along the shortest path of the level's navigation graph.

        The walk starts at the point nearest the player and ends at the point
        nearest goal, then walks the last stretch straight, stopping as soon as
        the player is within radius of goal or until holds for him. It follows
        walk paths on foot and mantles over low cover where the graph does.
        Refuses, naming the point, as walk does, or when the graph has no way
        there.
        """

        def arrived(player: Player) -> bool:
            return (distance(player.position, goal[:2]) <= radius
                    or (until is not None and until(player)))


        begin = self.alive(step)
        try:
            graph = NavigationGraph.from_json(self._pad.navigation())
            here = (*begin.position, begin.height)
            hops = graph.shortest_path(graph.nearest(here, NAV_HEIGHT).id,
                                       graph.nearest(goal, NAV_HEIGHT).id, TRAVEL_PATH_KINDS)
        except (ControlError, NavigationError) as error:
            raise RouteFailure(f"{step}: {error}") from error
        # The nearest point may lie behind the player; skip it when the next
        # one is already closer and is reached on foot.
        if len(hops) > 1 and hops[1].kind == "walk" and (
                distance(begin.position, hops[1].point.location[:2])
                < distance(hops[0].point.location[:2], hops[1].point.location[:2])):
            hops = hops[1:]
        for index, hop in enumerate(hops):
            # A goal such as a squad mate may stand short of a point, even
            # beside one the walk cannot reach.
            if arrived(self.alive(step)):
                break
            name = f"{step}: point {index + 1} of {len(hops)}"
            target = hop.point.location[:2]
            if hop.kind == "mantle":
                self.mantle(f"{name}, mantling", target)
            self.walk(name, target, radius=NAV_POINT_RADIUS, timeout=NAV_POINT_SECONDS,
                      unblock=unblock, until=arrived)
        if not arrived(self.alive(step)):
            self.walk(f"{step}: last stretch", goal[:2], radius=radius,
                      timeout=NAV_POINT_SECONDS, unblock=unblock, until=arrived)
        stopped = until is not None and until(self.alive(step))
        self._record(step, begin, goal=goal, points=len(hops),
                     mantles=sum(hop.kind == "mantle" for hop in hops), stopped=stopped)

    def mantle(self, step: str, goal: tuple[float, float]) -> None:
        """Cross low cover toward goal: A takes cover, and A again vaults it."""

        begin = self.alive(step)
        try:
            for buttons, seconds in MANTLE_SEQUENCE:
                until = self._clock() + seconds
                while self._clock() < until:
                    player = self.alive(step)
                    lx, ly = stick_toward(player.position, player.control_yaw, goal)
                    fields = {"lx": str(lx), "ly": str(ly)}
                    if buttons:
                        fields["buttons"] = buttons
                    self._pad.set_pad(fields)
                    self._sleep(POLL_SECONDS / 2)
        finally:
            self._pad.release()
        self._record(step, begin, goal=goal)

    def wait(self, seconds: float) -> None:
        self._sleep(seconds)

    def pressing(self, fields: dict[str, str], seconds: float) -> Callable[[], None]:
        """An unblock action that holds pad fields for seconds."""

        def hold() -> None:
            self._pad.set_pad(fields)
            self._sleep(seconds)
            self._pad.release()

        return hold

    def holding(self, buttons: str, seconds: float) -> Callable[[], None]:
        """An unblock action that holds buttons for seconds."""

        return self.pressing({"buttons": buttons}, seconds)

    def fire(self, step: str, seconds: float, attempts: int,
             before_each: Callable[[], None] | None = None) -> int:
        """Aim and fire until the weapon counts rounds; returns the change in its count.

        A tutorial prompt can hold the trigger until its button is pressed
        under it, so each attempt runs before_each first. Refuses when no
        attempt fired.
        """

        begin = self.alive(step)
        if begin.rounds_fired is None:
            raise RouteFailure(f"{step}: the player holds no weapon")
        for attempt in range(1, attempts + 1):
            if before_each is not None:
                before_each()
            before = self.alive(step)
            self._pad.set_pad({"lt": "255"})
            self._sleep(0.8)
            self._pad.set_pad({"lt": "255", "rt": "255"})
            self._sleep(seconds)
            self._pad.release()
            end = self.alive(step)
            # A reload during the burst restarts the magazine count; either change is a shot.
            if end.rounds_fired != before.rounds_fired:
                self._record(step, begin, attempts=attempt,
                             rounds_before=before.rounds_fired,
                             rounds_after=end.rounds_fired)
                return end.rounds_fired - before.rounds_fired
        raise RouteFailure(f"{step}: the weapon counted no rounds in {attempts} attempts")


    def respawn(self, step: str, timeout: float) -> None:
        """Load the last checkpoint from the death screen and wait for the new pawn.

        The death screen offers Load Last Checkpoint first, so A selects it;
        A is pressed only while the player has no pawn. Refuses when no pawn
        appears within timeout.
        """

        self._close_span()
        start = self._clock()
        while self.player().position is None:
            if self._clock() - start > timeout:
                raise RouteFailure(f"{step}: no pawn within {timeout:.0f} s of the death screen")
            self.holding("A", 0.3)()
            self._sleep(RESPAWN_POLL_SECONDS)
        # The checkpoint's opening camera holds the pawn briefly.
        self._sleep(RESPAWN_SETTLE_SECONDS)
        self._open_span()
        self._misfires.clear()
        self._dry.clear()
        player = self.player()
        self.steps.append({"step": step, "to": player.position,
                           "seconds": round(self._clock() - start, 1)})

    def clear_firefight(self, step: str, timeout: float, arrival: float,
                        cover: bool = False) -> int:
        """Fight from cover until no living hostile is listed; returns the hostiles fought.

        Waits up to arrival seconds for hostiles to appear. With cover, the
        player first takes a cover slot (take_cover), and leaves a slot for
        another, never to return, when he loses health while waiting in it:
        its cover no longer faces where the hostiles shoot from. Without
        cover, he fights from where he stands, in cover already. Each round turns
        the view onto the nearest hostile from cover, raises the weapon with
        LT only to correct the aim and fire a burst, and drops back into
        cover; below RECOVER_HEALTH it waits in cover instead. An empty
        weapon is reloaded or swapped (_after_burst). A target that
        takes no damage from BURSTS_PER_TARGET bursts yields to the others.
        The step's record lists every burst, also when the step fails.
        Refuses when the player dies, no hostile ever appears, or hostiles
        remain at the timeout.
        """

        begin = self.alive(step)
        start = self._clock()
        while not begin.hostiles():
            if self._clock() - start > arrival:
                raise RouteFailure(f"{step}: no hostile appeared within {arrival:.0f} s")
            self._sleep(POLL_SECONDS)
            begin = self.alive(step)
        seen = {pawn.id for pawn in begin.hostiles()}
        misses: dict[int, int] = {}
        bursts: list[dict[str, int | None]] = []
        slot: Point | None = None
        flanked: set[int] = set()
        try:
            while True:
                player = self.alive(step)
                hostiles = player.hostiles()
                seen.update(pawn.id for pawn in hostiles)
                if not hostiles:
                    break
                if self._clock() - start > timeout:
                    raise RouteFailure(
                        f"{step}: {len(hostiles)} hostile(s) still standing after {timeout:.0f} s"
                    )
                if cover:
                    slot = self.take_cover(f"{step}: take cover")
                    cover = False
                    continue
                close = [pawn for pawn in hostiles
                         if distance(player.position, pawn.location[:2]) <= CLOSE_RANGE]
                if player.health < RECOVER_HEALTH and not close:
                    self._pad.release()
                    self._sleep(COVER_SECONDS)
                    if slot is not None and self.alive(step).health < player.health:
                        flanked.add(slot.id)
                        slot = self.take_cover(f"{step}: leave flanked cover", flanked)
                    continue
                target = min(hostiles, key=lambda pawn: (
                    misses.get(pawn.id, 0) // BURSTS_PER_TARGET,
                    distance(player.position, pawn.location[:2])))
                after, fired, rounds = self._burst(step, target)
                bursts.append({"target": target.id, "health": target.health,
                               "after": None if after is None else after.health,
                               "own_health": player.health, "fired": fired,
                               "rounds_fired": rounds})
                if after is not None and after.health == target.health:
                    misses[target.id] = misses.get(target.id, 0) + 1
        except RouteFailure as failure:
            self._record(step, begin, failure=str(failure), hostiles=len(seen), bursts=bursts)
            raise
        finally:
            self._pad.release()
        self._record(step, begin, hostiles=len(seen), bursts=bursts,
                     seconds=round(self._clock() - start, 1))
        return len(seen)

    def take_cover(self, step: str, avoid: Collection[int] = ()) -> Point | None:
        """Take the cover slot choose_cover picks, other than those in avoid.

        Travels to the slot unless already at it, turns the view along the
        slot's facing so that A takes the cover beside it, and presses A.
        Returns the slot, or None, recording how many slots the level has,
        when no slot qualifies; the player then fights from where he stands.
        """

        begin = self.alive(step)
        hostiles = begin.hostiles()
        try:
            graph = NavigationGraph.from_json(self._pad.navigation())
        except (ControlError, NavigationError) as error:
            raise RouteFailure(f"{step}: {error}") from error
        slot = choose_cover(graph, begin, hostiles, avoid)
        if slot is None:
            self._record(step, begin, cover=None, slots=len(graph.of_kind("cover")))
            return None
        if distance(begin.position, slot.location[:2]) > COVER_RADIUS:
            self.travel(f"{step}: reach the slot", slot.location, radius=COVER_RADIUS,
                        unblock=self.unstick)
        radians = slot.yaw * 2.0 * math.pi / YAW_UNITS_PER_TURN
        ahead = (slot.location[0] + COVER_FACING_DISTANCE * math.cos(radians),
                 slot.location[1] + COVER_FACING_DISTANCE * math.sin(radians),
                 slot.location[2] - AIM_HEIGHT)
        self._aim(step, ahead, {})
        self.holding("A", 0.3)()
        self._sleep(COVER_SETTLE_SECONDS)
        self._record(step, begin, cover=slot.id, at=slot.location)
        return slot

    def _burst(self, step: str, target: Pawn
               ) -> tuple[Pawn | None, bool, tuple[int | None, int | None]]:
        """Aim at target from cover, fire one burst, and return to cover.

        A target within CLOSE_RANGE is fired at for CLOSE_BURST_SECONDS with
        the aim following it. Returns the target as listed afterwards (None
        once it has left the list), whether the weapon fired, and its
        rounds-fired count before and after the burst (None without a weapon).
        """

        self._hold_live_weapon(step)
        self._aim(step, target.location, {})
        self._aim(step, target.location, {"lt": "255"})
        start = self.alive(step)
        before = start.weapon
        firing = {"lt": "255", "rt": "255"}
        if distance(start.position, target.location[:2]) <= CLOSE_RANGE:
            self._track(step, target, firing, CLOSE_BURST_SECONDS)
        else:
            self._pad.set_pad(firing)
            self._sleep(BURST_SECONDS)
        self._pad.release()
        end = self.alive(step)
        after_burst = end.weapon
        fired = (before is not None and after_burst is not None
                 and after_burst.id == before.id
                 and after_burst.rounds_fired != before.rounds_fired)
        self._sleep(COVER_SECONDS)
        if before is not None:
            self._after_burst(step, before.id, fired)
        after = next((pawn for pawn in self.alive(step).pawns if pawn.id == target.id), None)
        rounds = (None if before is None else before.rounds_fired,
                  None if after_burst is None else after_burst.rounds_fired)
        return after, fired, rounds

    def dismiss_prompt(self) -> None:
        """Press A, which dismisses a tutorial prompt blocking the player's input."""

        self.holding("A", 0.3)()
        self._sleep(PROMPT_DISMISS_SECONDS)

    def unstick(self) -> None:
        """An unblock for a walk: dismiss a prompt, then step back out of cover.

        A with no prompt takes cover at a wall, which the step back leaves.
        """

        self.dismiss_prompt()
        self.pressing({"ly": str(-STICK_LIMIT)}, LEAVE_COVER_SECONDS)()

    def revive(self, step: str, mate: Pawn) -> None:
        """Walk to a downed squad mate and press X beside him until he stands.

        A press that does not revive him is followed by A, in case the
        REVIVE prompt blocks X. Refuses when he is still down after
        REVIVE_PRESSES presses.
        """

        begin = self.alive(step)
        self.travel(step, mate.location, radius=REVIVE_RADIUS, unblock=self.unstick)
        for press in range(1, REVIVE_PRESSES + 1):
            self.holding("X", 0.3)()
            self._sleep(REVIVE_PRESS_SECONDS)
            listed = next((pawn for pawn in self.alive(step).pawns if pawn.id == mate.id), None)
            if listed is not None and listed.health > 0:
                self._record(step, begin, mate=mate.id, presses=press)
                return
            self.dismiss_prompt()
        raise RouteFailure(f"{step}: the squad mate was still down after {REVIVE_PRESSES} "
                           "presses of X")

    def _after_burst(self, step: str, weapon: int, fired: bool) -> None:
        """Apply the next misfire remedy to a weapon whose burst fired nothing.

        A weapon that fired nothing after every remedy is marked dry.
        """

        if fired:
            self._misfires.pop(weapon, None)
            return
        misfires = self._misfires.get(weapon, 0)
        if misfires == len(MISFIRE_REMEDIES):
            self._dry.add(weapon)
            self.steps.append({"step": f"{step}: weapon {weapon} is dry"})
            return
        remedy = MISFIRE_REMEDIES[misfires]
        self._misfires[weapon] = misfires + 1
        self.steps.append({"step": f"{step}: weapon {weapon} fired nothing; press {remedy}"})
        if remedy == "A":
            self.dismiss_prompt()
        else:
            self.holding(remedy, 0.3)()
            self._sleep(RELOAD_SECONDS)

    def _hold_live_weapon(self, step: str) -> None:
        """Hold a weapon not known to be dry, trying the d-pad slots in turn.

        Refuses when the player holds no weapon or every slot holds a dry one.
        """

        weapon = self.alive(step).weapon
        if weapon is None:
            raise RouteFailure(f"{step}: the player holds no weapon")
        if weapon.id not in self._dry:
            return
        for slot in WEAPON_SLOTS:
            self.holding(slot, 0.3)()
            self._sleep(SWITCH_SECONDS)
            weapon = self.alive(step).weapon
            if weapon is not None and weapon.id not in self._dry:
                self.steps.append({"step": f"{step}: switch to the weapon on {slot}",
                                   "weapon": weapon.id})
                return
        raise RouteFailure(f"{step}: every weapon is out of ammunition")

    def _track(self, step: str, target: Pawn, held: dict[str, str], seconds: float) -> None:
        """Hold held pad fields for seconds, turning the view after target as it moves."""

        start = self._clock()
        while self._clock() - start < seconds:
            player = self.alive(step)
            listed = next((pawn for pawn in player.pawns if pawn.id == target.id), None)
            if listed is None or listed.health <= 0:
                return
            yaw_error, pitch_error = aim_error(player, listed.location)
            self._pad.set_pad({**held, "rx": str(aim_stick(yaw_error)),
                               "ry": str(aim_stick(pitch_error))})
            self._sleep(AIM_POLL_SECONDS)

    def _aim(self, step: str, target: tuple[float, float, float], held: dict[str, str]) -> None:
        """Turn the view onto a point above target with held pad fields, for at most AIM_SECONDS."""

        start = self._clock()
        while self._clock() - start < AIM_SECONDS:
            yaw_error, pitch_error = aim_error(self.alive(step), target)
            if abs(yaw_error) <= AIM_TOLERANCE and abs(pitch_error) <= AIM_TOLERANCE:
                return
            self._pad.set_pad({**held, "rx": str(aim_stick(yaw_error)),
                               "ry": str(aim_stick(pitch_error))})
            self._sleep(AIM_POLL_SECONDS)


# Positions measured on the retail image's sp_prison_p (docs/re-frontier.md).
# Where the walk usually leaves Marcus: in cover past the path choice.

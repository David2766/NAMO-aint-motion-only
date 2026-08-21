from __future__ import annotations

from dataclasses import dataclass, replace
from ipaddress import IPv4Address, AddressValueError
import math
import re
from typing import Any, Mapping
from urllib.parse import urlsplit

from .const import MAX_AUTOMATION_AGE_MS, MAX_CONTINUITY_HOLD_MS, MAX_SITE_GROUPS


class RouteContractError(ValueError):
    """Raised when a device violates the bounded route contract."""


@dataclass(frozen=True, slots=True)
class GroupRoute:
    site_id: str
    space_id: str
    group_id: str
    leader_node_id: str
    group_term: int
    base_url: str | None
    state_path: str
    available: bool
    display_name: str = ""
    standalone: bool = False
    participants_online: int = 0
    participants_expected: int = 0
    group_health: str = "unavailable"


@dataclass(frozen=True, slots=True)
class AreaSnapshot:
    area_id: str
    name: str
    presence: bool | None
    target_count: int | None


@dataclass(frozen=True, slots=True)
class SpaceSnapshot:
    site_id: str
    space_id: str
    group_id: str
    leader_node_id: str
    group_term: int
    available: bool
    health: str
    presence: bool | None
    motion: bool | None
    target_count: int | None
    boot_id: str | None
    sequence: int
    source_sequence: int
    floorplan_id: str | None
    areas_revision: int
    areas_available: bool
    areas: tuple[AreaSnapshot, ...]
    display_name: str = ""
    unavailable_reason: str | None = None
    standalone: bool = False
    participants_online: int = 0
    participants_expected: int = 0
    source_age_ms: int | None = None
    continuity_held: bool = False
    hold_remaining_ms: int | None = None


@dataclass(frozen=True, slots=True)
class SiteSnapshot:
    site_id: str
    health: str
    spaces_available: int
    spaces_total: int
    devices_online: int
    devices_expected: int
    problem_spaces: tuple[str, ...]


@dataclass(frozen=True, slots=True)
class DirectorySnapshot:
    site_id: str
    leader_node_id: str
    routes: tuple[GroupRoute, ...]


@dataclass(frozen=True, slots=True)
class FloorplanSpaces:
    names: Mapping[str, str]
    radar_space_id: str | None


def normalize_device_url(value: str) -> str:
    raw = value.strip()
    if not raw:
        raise RouteContractError("device address is empty")
    if "://" not in raw:
        raw = f"http://{raw}"
    parsed = urlsplit(raw)
    if parsed.scheme != "http" or not parsed.hostname:
        raise RouteContractError("device address must use HTTP")
    if parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise RouteContractError("device address contains unsupported fields")
    try:
        port = parsed.port
    except ValueError as err:
        raise RouteContractError("device port is invalid") from err
    host = parsed.hostname
    authority = host if port in (None, 80) else f"{host}:{port}"
    return f"http://{authority}"


def participant_urls_from_payload(payload: Mapping[str, Any]) -> tuple[str, ...]:
    records = payload.get("participants")
    if not isinstance(records, list):
        raise RouteContractError("participant directory is unavailable")
    urls: list[str] = []
    for record in records:
        if (
            not isinstance(record, Mapping)
            or record.get("canonicalMember") is not True
        ):
            continue
        endpoint = record.get("endpoint")
        if not isinstance(endpoint, Mapping):
            continue
        address = endpoint.get("address")
        port = endpoint.get("port", 80)
        if not isinstance(address, str) or not address:
            continue
        if (
            not isinstance(port, int)
            or isinstance(port, bool)
            or not 1 <= port <= 65535
        ):
            continue
        authority = address if port == 80 else f"{address}:{port}"
        try:
            urls.append(normalize_device_url(authority))
        except RouteContractError:
            continue
    return tuple(dict.fromkeys(urls))


def management_candidate_urls(
    seed_url: str,
    leader_url: str | None,
    route_urls: tuple[str | None, ...],
    participant_urls: tuple[str, ...],
) -> tuple[str, ...]:
    values = [seed_url]
    if leader_url is not None:
        values.append(leader_url)
    values.extend(value for value in route_urls if value is not None)
    values.extend(participant_urls)
    return tuple(dict.fromkeys(values))


def _text(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise RouteContractError(f"{field} is missing")
    return value


def _integer(value: Any, field: str, *, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise RouteContractError(f"{field} is invalid")
    return value


def _optional_bool(value: Any, field: str) -> bool | None:
    if value is None:
        return None
    if not isinstance(value, bool):
        raise RouteContractError(f"{field} is invalid")
    return value


def _optional_count(value: Any, field: str) -> int | None:
    if value is None:
        return None
    return _integer(value, field)


def _bounded_assignment_id(value: str) -> str:
    if re.fullmatch(r"[A-Za-z0-9._:-]{1,40}", value):
        return value
    hash_value = 2166136261
    encoded = value.encode("utf-16-le", errors="surrogatepass")
    for index in range(0, len(encoded), 2):
        code_unit = encoded[index] | (encoded[index + 1] << 8)
        hash_value ^= code_unit
        hash_value = (hash_value * 16777619) & 0xFFFFFFFF
    return f"room-{hash_value:08x}"


def _point_in_polygon(x: float, y: float, points: list[Any]) -> bool:
    normalized: list[tuple[float, float]] = []
    for point in points:
        if (
            not isinstance(point, list)
            or len(point) != 2
            or isinstance(point[0], bool)
            or isinstance(point[1], bool)
            or not isinstance(point[0], (int, float))
            or not isinstance(point[1], (int, float))
        ):
            return False
        normalized.append((float(point[0]), float(point[1])))
    if len(normalized) < 3:
        return False

    inside = False
    previous_x, previous_y = normalized[-1]
    for current_x, current_y in normalized:
        cross = (x - previous_x) * (current_y - previous_y) - (
            y - previous_y
        ) * (current_x - previous_x)
        if abs(cross) <= 1e-6 and min(previous_x, current_x) <= x <= max(
            previous_x, current_x
        ) and min(previous_y, current_y) <= y <= max(previous_y, current_y):
            return True
        intersects = (current_y > y) != (previous_y > y)
        if intersects:
            intersection_x = (
                (previous_x - current_x)
                * (y - current_y)
                / (previous_y - current_y)
                + current_x
            )
            if x < intersection_x:
                inside = not inside
        previous_x, previous_y = current_x, current_y
    return inside


def parse_floorplan_spaces(payload: Mapping[str, Any]) -> FloorplanSpaces:
    raw_rooms = payload.get("rooms")
    if not isinstance(raw_rooms, list):
        return FloorplanSpaces({}, None)

    rooms: list[tuple[str, str, list[Any]]] = []
    names: dict[str, str] = {}
    for raw_room in raw_rooms:
        if not isinstance(raw_room, Mapping):
            continue
        room_id = raw_room.get("id")
        room_name = raw_room.get("name")
        points = raw_room.get("pointsPx")
        if (
            not isinstance(room_id, str)
            or not room_id
            or not isinstance(room_name, str)
            or not room_name
            or not isinstance(points, list)
        ):
            continue
        bounded_id = _bounded_assignment_id(room_id)
        names[bounded_id] = room_name
        rooms.append((bounded_id, room_name, points))

    radar = payload.get("radar")
    scale = payload.get("scale")
    radar_space_id: str | None = None
    if isinstance(radar, Mapping) and isinstance(scale, Mapping):
        origin = radar.get("originPx")
        rotation = radar.get("rotationDeg")
        mm_per_px_x = scale.get("mmPerPxX")
        mm_per_px_y = scale.get("mmPerPxY")
        if (
            isinstance(origin, list)
            and len(origin) == 2
            and all(isinstance(value, (int, float)) for value in origin)
            and isinstance(rotation, (int, float))
            and isinstance(mm_per_px_x, (int, float))
            and isinstance(mm_per_px_y, (int, float))
            and mm_per_px_x > 0
            and mm_per_px_y > 0
        ):
            angle = math.radians(float(rotation))
            probes = (
                (
                    float(origin[0]) + 250.0 * math.sin(angle) / mm_per_px_x,
                    float(origin[1]) - 250.0 * math.cos(angle) / mm_per_px_y,
                ),
                (float(origin[0]), float(origin[1])),
            )
            for probe_x, probe_y in probes:
                match = next(
                    (
                        room_id
                        for room_id, _name, points in rooms
                        if _point_in_polygon(probe_x, probe_y, points)
                    ),
                    None,
                )
                if match is not None:
                    radar_space_id = match
                    break
    if radar_space_id is None and len(rooms) == 1:
        radar_space_id = rooms[0][0]
    return FloorplanSpaces(dict(names), radar_space_id)


def standalone_route(
    *,
    site_id: str,
    node_id: str,
    base_url: str,
    device_name: str,
    floorplan: FloorplanSpaces,
) -> GroupRoute:
    space_id = floorplan.radar_space_id or f"standalone:{node_id}"
    return GroupRoute(
        site_id=site_id,
        space_id=space_id,
        group_id=f"standalone:{node_id}",
        leader_node_id=node_id,
        group_term=1,
        base_url=normalize_device_url(base_url),
        state_path="/api/state",
        available=True,
        display_name=floorplan.names.get(space_id, device_name),
        standalone=True,
        participants_online=1,
        participants_expected=1,
        group_health="healthy",
    )


def resolve_site_routes(
    directory: DirectorySnapshot,
    *,
    mode: str,
    node_id: str,
    base_url: str,
    device_name: str,
    floorplan: FloorplanSpaces,
) -> tuple[GroupRoute, ...]:
    if directory.routes:
        return tuple(
            replace(
                route,
                display_name=floorplan.names.get(
                    route.space_id, route.space_id
                ),
            )
            for route in directory.routes
        )
    if mode != "standalone":
        return ()
    return (
        standalone_route(
            site_id=directory.site_id,
            node_id=node_id,
            base_url=base_url,
            device_name=device_name,
            floorplan=floorplan,
        ),
    )


def _remote_base_url(address: Any, port: Any) -> str:
    address_text = _text(address, "route.address")
    try:
        IPv4Address(address_text)
    except AddressValueError as err:
        raise RouteContractError("route.address is not IPv4") from err
    port_value = _integer(port, "route.port", minimum=1)
    if port_value > 65535:
        raise RouteContractError("route.port is invalid")
    authority = address_text if port_value == 80 else f"{address_text}:{port_value}"
    return f"http://{authority}"


def parse_directory(
    *,
    leader_url: str,
    site_id: str,
    groups_payload: Mapping[str, Any],
) -> DirectorySnapshot:
    leader_url = normalize_device_url(leader_url)
    site_id = _text(site_id, "siteId")
    site = groups_payload.get("site")
    summary = groups_payload.get("siteGroupSummary")
    entries = groups_payload.get("siteGroups")
    if not isinstance(site, Mapping) or site.get("localLeader") is not True:
        raise RouteContractError("directory source is not the current coordinator")
    if not isinstance(summary, Mapping) or summary.get("localAuthority") is not True:
        raise RouteContractError("directory source has no authority")
    capacity = _integer(summary.get("capacity"), "siteGroupSummary.capacity")
    if capacity > MAX_SITE_GROUPS:
        raise RouteContractError("directory capacity exceeds the client bound")
    configured_groups = _integer(
        summary.get("configuredGroups"),
        "siteGroupSummary.configuredGroups",
    )
    reporting_groups = _integer(
        summary.get("reportingGroups"),
        "siteGroupSummary.reportingGroups",
    )
    if configured_groups > capacity or reporting_groups > configured_groups:
        raise RouteContractError("directory Group counts are invalid")
    if not isinstance(entries, list) or len(entries) > MAX_SITE_GROUPS:
        raise RouteContractError("siteGroups exceeds the client bound")
    if len(entries) != configured_groups:
        raise RouteContractError("configured Site Groups are incomplete")

    leader_node_id = _text(site.get("leaderNodeId"), "site.leaderNodeId")
    routes: list[GroupRoute] = []
    seen_spaces: set[str] = set()
    seen_groups: set[str] = set()
    for entry in entries:
        if not isinstance(entry, Mapping):
            raise RouteContractError("siteGroups entry is invalid")
        group_id = _text(entry.get("groupId"), "siteGroups.groupId")
        space_id = _text(entry.get("spaceId"), "siteGroups.spaceId")
        group_leader = _text(
            entry.get("leaderNodeId"), "siteGroups.leaderNodeId"
        )
        term = _integer(entry.get("term"), "siteGroups.term", minimum=1)
        participants_online = _integer(
            entry.get("participantsOnline"),
            "siteGroups.participantsOnline",
        )
        participants_expected = _integer(
            entry.get("participantsExpected"),
            "siteGroups.participantsExpected",
            minimum=1,
        )
        if participants_online > participants_expected:
            raise RouteContractError(
                "siteGroups participantsOnline exceeds participantsExpected"
            )
        group_health = _text(entry.get("health"), "siteGroups.health")
        if group_health not in {"healthy", "degraded", "unavailable"}:
            raise RouteContractError("siteGroups.health is invalid")
        if group_id in seen_groups or space_id in seen_spaces:
            raise RouteContractError("directory contains duplicate logical identity")
        seen_groups.add(group_id)
        seen_spaces.add(space_id)

        route = entry.get("route")
        if not isinstance(route, Mapping):
            raise RouteContractError("siteGroups.route is missing")
        if route.get("schemaVersion") != 1:
            raise RouteContractError("route schema is unsupported")
        available = route.get("available") is True
        state_path = route.get("statePath")
        if state_path != "/api/state":
            raise RouteContractError("route.statePath is invalid")
        base_url: str | None = None
        if available:
            if route.get("local") is True:
                if group_leader != leader_node_id:
                    raise RouteContractError("local route leader does not match")
                base_url = leader_url
            else:
                base_url = _remote_base_url(
                    route.get("address"), route.get("port")
                )
        routes.append(
            GroupRoute(
                site_id=site_id,
                space_id=space_id,
                group_id=group_id,
                leader_node_id=group_leader,
                group_term=term,
                base_url=base_url,
                state_path=state_path,
                available=available,
                display_name=space_id,
                participants_online=participants_online,
                participants_expected=participants_expected,
                group_health=group_health,
            )
        )
    return DirectorySnapshot(site_id, leader_node_id, tuple(routes))


def unavailable_snapshot(
    route: GroupRoute,
    previous: SpaceSnapshot | None = None,
    *,
    health: str = "unavailable",
    reason: str = "unavailable",
) -> SpaceSnapshot:
    floorplan_id = None
    areas_revision = 0
    areas = ()
    if previous is not None:
        floorplan_id = previous.floorplan_id
        areas_revision = previous.areas_revision
        areas = tuple(
            replace(area, presence=None, target_count=None)
            for area in previous.areas
        )
    return SpaceSnapshot(
        site_id=route.site_id,
        space_id=route.space_id,
        group_id=route.group_id,
        leader_node_id=route.leader_node_id,
        group_term=route.group_term,
        available=False,
        health=health,
        presence=None,
        motion=None,
        target_count=None,
        boot_id=None,
        sequence=0,
        source_sequence=0,
        floorplan_id=floorplan_id,
        areas_revision=areas_revision,
        areas_available=False,
        areas=areas,
        display_name=route.display_name,
        unavailable_reason=reason,
        standalone=route.standalone,
        participants_online=route.participants_online,
        participants_expected=route.participants_expected,
        source_age_ms=None,
    )


def _parse_areas(
    payload: Mapping[str, Any],
    field: str,
    previous: SpaceSnapshot | None,
) -> tuple[str | None, int, bool, tuple[AreaSnapshot, ...]]:
    floorplan_id = payload.get("floorplanId")
    revision = payload.get("areasRevision")
    available = payload.get("areasAvailable")
    raw_areas = payload.get("areas")
    contract_missing = (
        floorplan_id is None
        and revision is None
        and available is None
        and raw_areas is None
    )
    if contract_missing:
        if previous is None:
            return None, 0, False, ()
        return (
            previous.floorplan_id,
            previous.areas_revision,
            False,
            tuple(
                replace(area, presence=None, target_count=None)
                for area in previous.areas
            ),
        )
    no_floorplan = (
        floorplan_id is None
        and isinstance(revision, int)
        and not isinstance(revision, bool)
        and revision == 0
        and available is False
        and isinstance(raw_areas, list)
        and not raw_areas
    )
    if no_floorplan:
        return None, 0, False, ()
    if not isinstance(floorplan_id, str) or not floorplan_id:
        raise RouteContractError(f"{field}.floorplanId is invalid")
    revision = _integer(revision, f"{field}.areasRevision")
    if not isinstance(available, bool):
        raise RouteContractError(f"{field}.areasAvailable is invalid")
    if not isinstance(raw_areas, list) or len(raw_areas) > 6:
        raise RouteContractError(f"{field}.areas is invalid")

    areas: list[AreaSnapshot] = []
    seen: set[str] = set()
    for raw_area in raw_areas:
        if not isinstance(raw_area, Mapping):
            raise RouteContractError(f"{field}.area is invalid")
        area_id = _text(raw_area.get("areaId"), f"{field}.areaId")
        if area_id in seen:
            raise RouteContractError(f"{field}.area identity is duplicated")
        seen.add(area_id)
        presence = _optional_bool(
            raw_area.get("presence"), f"{field}.area.presence"
        )
        target_count = _optional_count(
            raw_area.get("targetCount"), f"{field}.area.targetCount"
        )
        if available and (presence is None or target_count is None):
            raise RouteContractError(
                f"{field}.available area values cannot be null"
            )
        areas.append(
            AreaSnapshot(
                area_id=area_id,
                name=_text(raw_area.get("name"), f"{field}.area.name"),
                presence=presence if available else None,
                target_count=target_count if available else None,
            )
        )
    return floorplan_id, revision, available, tuple(areas)


def parse_standalone_state(
    route: GroupRoute,
    payload: Mapping[str, Any],
    previous: SpaceSnapshot | None = None,
) -> SpaceSnapshot:
    if not route.standalone:
        raise RouteContractError("route is not standalone")
    if payload.get("version") != 1 or payload.get("connected") is not True:
        return unavailable_snapshot(
            route, previous, reason="device_disconnected"
        )
    presence = _optional_bool(payload.get("presence"), "state.presence")
    motion = _optional_bool(payload.get("motion"), "state.motion")
    target_count = _optional_count(payload.get("targetCount"), "state.targetCount")
    if presence is None or motion is None or target_count is None:
        raise RouteContractError("standalone state values are incomplete")
    updated_at = _integer(payload.get("updatedAt"), "state.updatedAt")
    floorplan_id, areas_revision, areas_available, areas = _parse_areas(
        payload, "state", previous
    )
    return SpaceSnapshot(
        site_id=route.site_id,
        space_id=route.space_id,
        group_id=route.group_id,
        leader_node_id=route.leader_node_id,
        group_term=route.group_term,
        available=True,
        health="healthy",
        presence=presence,
        motion=motion,
        target_count=target_count,
        boot_id=None,
        sequence=updated_at,
        source_sequence=updated_at,
        floorplan_id=floorplan_id,
        areas_revision=areas_revision,
        areas_available=areas_available,
        areas=areas,
        display_name=route.display_name,
        unavailable_reason=None,
        standalone=True,
        participants_online=1,
        participants_expected=1,
        source_age_ms=0,
    )


def parse_automation_state(
    route: GroupRoute,
    payload: Mapping[str, Any],
    previous: SpaceSnapshot | None = None,
) -> SpaceSnapshot:
    automation = payload.get("automation")
    if not isinstance(automation, Mapping):
        raise RouteContractError("automation snapshot is missing")
    if automation.get("schemaVersion") != 1 or automation.get("complete") is not True:
        raise RouteContractError("automation snapshot is incomplete")
    if automation.get("siteId") != route.site_id:
        raise RouteContractError("automation site authority does not match")
    if automation.get("spaceId") != route.space_id:
        raise RouteContractError("automation space authority does not match")
    if automation.get("groupId") != route.group_id:
        raise RouteContractError("automation group authority does not match")
    if automation.get("leaderNodeId") != route.leader_node_id:
        raise RouteContractError("automation leader authority does not match")
    term = _integer(automation.get("groupTerm"), "automation.groupTerm", minimum=1)
    if term != route.group_term:
        raise RouteContractError("automation term does not match")

    health = _text(automation.get("health"), "automation.health")
    continuity_held = automation.get("continuityHeld") is True
    available = automation.get("available") is True and (
        health == "healthy" or (health == "recovering" and continuity_held)
    )
    if not available:
        return unavailable_snapshot(
            route,
            previous,
            health=(
                health
                if health in {"degraded", "unavailable", "recovering"}
                else "unavailable"
            ),
            reason=(
                "automation_degraded"
                if health == "degraded"
                else "automation_unavailable"
            ),
        )

    boot_id = _text(automation.get("bootId"), "automation.bootId")
    sequence = _integer(automation.get("sequence"), "automation.sequence", minimum=1)
    source_sequence = _integer(
        automation.get("sourceSequence"), "automation.sourceSequence", minimum=1
    )
    age_ms = _integer(automation.get("ageMs"), "automation.ageMs")
    hold_remaining_ms: int | None = None
    hold_age_ms = 0
    if continuity_held:
        hold_age_ms = _integer(automation.get("holdAgeMs"), "automation.holdAgeMs")
        hold_remaining_ms = _integer(
            automation.get("holdRemainingMs"), "automation.holdRemainingMs"
        )
        _text(
            automation.get("continuitySourceLeaderNodeId"),
            "automation.continuitySourceLeaderNodeId",
        )
        continuity_source_term = _integer(
            automation.get("continuitySourceGroupTerm"),
            "automation.continuitySourceGroupTerm",
            minimum=1,
        )
        if (
            health != "recovering"
            or continuity_source_term > term
            or hold_age_ms > MAX_CONTINUITY_HOLD_MS
            or not 0 < hold_remaining_ms <= MAX_CONTINUITY_HOLD_MS
            or hold_age_ms + hold_remaining_ms > MAX_CONTINUITY_HOLD_MS + 1000
            or age_ms < hold_age_ms
        ):
            raise RouteContractError("automation continuity hold is invalid")
    maximum_age_ms = MAX_AUTOMATION_AGE_MS + hold_age_ms
    if age_ms > maximum_age_ms:
        raise RouteContractError("automation snapshot is stale")
    presence = _optional_bool(automation.get("presence"), "automation.presence")
    motion = _optional_bool(automation.get("motion"), "automation.motion")
    target_count = _optional_count(
        automation.get("targetCount"), "automation.targetCount"
    )
    if presence is None or motion is None or target_count is None:
        raise RouteContractError("available automation values cannot be null")

    floorplan_id, areas_revision, areas_available, areas = _parse_areas(
        automation, "automation", previous
    )
    effective_health = (
        "degraded"
        if continuity_held
        else (
            "healthy"
            if route.group_health == "healthy"
            and route.participants_online == route.participants_expected
            else "degraded"
        )
    )
    return SpaceSnapshot(
        site_id=route.site_id,
        space_id=route.space_id,
        group_id=route.group_id,
        leader_node_id=route.leader_node_id,
        group_term=term,
        available=True,
        health=effective_health,
        presence=presence,
        motion=motion,
        target_count=target_count,
        boot_id=boot_id,
        sequence=sequence,
        source_sequence=source_sequence,
        floorplan_id=floorplan_id,
        areas_revision=areas_revision,
        areas_available=areas_available,
        areas=areas,
        display_name=route.display_name,
        unavailable_reason=("automation_recovering" if continuity_held else None),
        standalone=False,
        participants_online=route.participants_online,
        participants_expected=route.participants_expected,
        source_age_ms=age_ms,
        continuity_held=continuity_held,
        hold_remaining_ms=hold_remaining_ms,
    )


def summarize_site(
    site_id: str, spaces: Mapping[str, SpaceSnapshot]
) -> SiteSnapshot:
    snapshots = tuple(spaces.values())
    spaces_total = len(snapshots)
    spaces_available = sum(snapshot.available for snapshot in snapshots)
    devices_online = sum(snapshot.participants_online for snapshot in snapshots)
    devices_expected = sum(
        snapshot.participants_expected for snapshot in snapshots
    )
    problems = tuple(
        snapshot.display_name or snapshot.space_id
        for snapshot in snapshots
        if not snapshot.available
        or snapshot.health != "healthy"
        or snapshot.participants_online < snapshot.participants_expected
    )
    if spaces_total == 0 or spaces_available == 0:
        health = "unavailable"
    elif problems:
        health = "degraded"
    else:
        health = "healthy"
    return SiteSnapshot(
        site_id=site_id,
        health=health,
        spaces_available=spaces_available,
        spaces_total=spaces_total,
        devices_online=devices_online,
        devices_expected=devices_expected,
        problem_spaces=problems,
    )


class SnapshotFence:
    """Retain only authoritative, non-reordered Space snapshots."""

    def __init__(self) -> None:
        self._accepted: dict[str, SpaceSnapshot] = {}

    def previous(self, space_id: str) -> SpaceSnapshot | None:
        return self._accepted.get(space_id)

    def accept(self, snapshot: SpaceSnapshot) -> SpaceSnapshot:
        previous = self._accepted.get(snapshot.space_id)
        if not snapshot.available:
            return snapshot
        if previous is not None and previous.available:
            same_boot_authority = (
                snapshot.leader_node_id == previous.leader_node_id
                and snapshot.boot_id == previous.boot_id
            )
            if same_boot_authority and snapshot.group_term < previous.group_term:
                raise RouteContractError("automation term moved backwards")
            same_authority = (
                snapshot.group_term == previous.group_term
                and same_boot_authority
            )
            if same_authority:
                if snapshot.sequence < previous.sequence:
                    raise RouteContractError("automation sequence moved backwards")
                if snapshot.sequence == previous.sequence:
                    previous_area_identity = (
                        previous.floorplan_id,
                        previous.areas_revision,
                        tuple(
                            (area.area_id, area.name)
                            for area in previous.areas
                        ),
                    )
                    next_area_identity = (
                        snapshot.floorplan_id,
                        snapshot.areas_revision,
                        tuple(
                            (area.area_id, area.name)
                            for area in snapshot.areas
                        ),
                    )
                    if next_area_identity == previous_area_identity:
                        return previous
                    if (
                        snapshot.floorplan_id == previous.floorplan_id
                        and snapshot.areas_revision
                        < previous.areas_revision
                    ):
                        raise RouteContractError(
                            "automation area revision moved backwards"
                        )
                if snapshot.source_sequence < previous.source_sequence:
                    raise RouteContractError(
                        "automation source sequence moved backwards"
                    )
        self._accepted[snapshot.space_id] = snapshot
        return snapshot

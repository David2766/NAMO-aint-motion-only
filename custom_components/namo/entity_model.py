from __future__ import annotations

from .routing import SpaceSnapshot


def space_device_identifier(site_id: str, space_id: str) -> str:
    return f"{site_id}:{space_id}"


def site_device_identifier(site_id: str) -> str:
    return f"site:{site_id}"


def space_device_name(snapshot: SpaceSnapshot | None, space_id: str) -> str:
    display_name = snapshot.display_name.strip() if snapshot is not None else ""
    return f"NAMO {display_name or space_id}"


def space_entity_unique_id(site_id: str, space_id: str, kind: str) -> str:
    return f"{site_id}:{space_id}:{kind}"


def area_entity_unique_id(
    site_id: str, floorplan_id: str, area_id: str, kind: str
) -> str:
    return f"{site_id}:{floorplan_id}:area:{area_id}:{kind}"


def legacy_site_status_unique_id(site_id: str) -> str:
    return f"{site_id}:site:status"


def is_legacy_zone_unique_id(unique_id: str) -> bool:
    return ":zone:" in unique_id and unique_id.endswith(":occupancy")

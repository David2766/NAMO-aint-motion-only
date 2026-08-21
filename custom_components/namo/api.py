from __future__ import annotations

import asyncio
from dataclasses import dataclass
from typing import Any, Mapping

from aiohttp import ClientError, ClientSession

from .const import REQUEST_TIMEOUT_SECONDS
from .routing import (
    RouteContractError,
    normalize_device_url,
    participant_urls_from_payload,
)


class NamoApiError(RuntimeError):
    """Raised when a NAMO endpoint cannot provide a valid response."""


@dataclass(frozen=True, slots=True)
class SiteLeaderResponse:
    base_url: str
    site_id: str
    node_id: str
    mode: str
    device_name: str
    groups: Mapping[str, Any]


class NamoApiClient:
    def __init__(self, session: ClientSession) -> None:
        self._session = session

    async def async_get_json(
        self, base_url: str, path: str
    ) -> Mapping[str, Any]:
        url = f"{normalize_device_url(base_url)}{path}"
        try:
            async with asyncio.timeout(REQUEST_TIMEOUT_SECONDS):
                async with self._session.get(url) as response:
                    if response.status != 200:
                        raise NamoApiError(f"GET {path} returned {response.status}")
                    payload = await response.json(content_type=None)
        except (TimeoutError, ClientError, ValueError, RouteContractError) as err:
            raise NamoApiError(f"GET {path} failed") from err
        if not isinstance(payload, Mapping):
            raise NamoApiError(f"GET {path} returned a non-object response")
        return payload

    async def async_resolve_site_leader(
        self, seed_url: str
    ) -> SiteLeaderResponse:
        seed_url = normalize_device_url(seed_url)
        system, groups = await asyncio.gather(
            self.async_get_json(seed_url, "/api/system/status"),
            self.async_get_json(seed_url, "/api/platform/groups"),
        )
        platform = system.get("platform")
        site = groups.get("site")
        if not isinstance(platform, Mapping) or not isinstance(site, Mapping):
            raise NamoApiError("platform identity is unavailable")
        site_id = platform.get("siteId")
        local_node_id = platform.get("nodeId")
        mode = platform.get("mode")
        leader_node_id = site.get("leaderNodeId")
        device = system.get("device")
        device_name = (
            device.get("name")
            if isinstance(device, Mapping) and isinstance(device.get("name"), str)
            else local_node_id
        )
        if not all(
            isinstance(value, str) and value
            for value in (site_id, local_node_id, leader_node_id, mode, device_name)
        ):
            raise NamoApiError("platform identity is incomplete")
        if site.get("localLeader") is True:
            if local_node_id != leader_node_id:
                raise NamoApiError("local coordinator identity does not match")
            return SiteLeaderResponse(
                seed_url,
                site_id,
                local_node_id,
                mode,
                device_name,
                groups,
            )

        participants = await self.async_get_json(
            seed_url, "/api/platform/participants"
        )
        records = participants.get("participants")
        if not isinstance(records, list):
            raise NamoApiError("participant directory is unavailable")
        leader_address: str | None = None
        for record in records:
            if not isinstance(record, Mapping):
                continue
            if record.get("participantId") != leader_node_id:
                continue
            endpoint = record.get("endpoint")
            if (
                isinstance(endpoint, Mapping)
                and endpoint.get("online") is True
                and isinstance(endpoint.get("address"), str)
            ):
                leader_address = endpoint["address"]
            break
        if leader_address is None:
            raise NamoApiError("current coordinator is not reachable")

        leader_url = normalize_device_url(leader_address)
        leader_system, leader_groups = await asyncio.gather(
            self.async_get_json(leader_url, "/api/system/status"),
            self.async_get_json(leader_url, "/api/platform/groups"),
        )
        leader_platform = leader_system.get("platform")
        leader_device = leader_system.get("device")
        leader_site = leader_groups.get("site")
        if not isinstance(leader_platform, Mapping) or not isinstance(
            leader_site, Mapping
        ):
            raise NamoApiError("coordinator identity is unavailable")
        if (
            leader_platform.get("siteId") != site_id
            or leader_platform.get("nodeId") != leader_node_id
            or leader_site.get("leaderNodeId") != leader_node_id
            or leader_site.get("localLeader") is not True
        ):
            raise NamoApiError("coordinator authority changed during discovery")
        leader_mode = leader_platform.get("mode")
        leader_device_name = (
            leader_device.get("name")
            if isinstance(leader_device, Mapping)
            else None
        )
        if not isinstance(leader_mode, str) or not leader_mode:
            raise NamoApiError("coordinator mode is unavailable")
        if not isinstance(leader_device_name, str) or not leader_device_name:
            leader_device_name = leader_node_id
        return SiteLeaderResponse(
            leader_url,
            site_id,
            leader_node_id,
            leader_mode,
            leader_device_name,
            leader_groups,
        )

    async def async_get_participant_urls(self, base_url: str) -> tuple[str, ...]:
        payload = await self.async_get_json(base_url, "/api/platform/participants")
        try:
            return participant_urls_from_payload(payload)
        except RouteContractError as err:
            raise NamoApiError(str(err)) from err

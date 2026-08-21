from __future__ import annotations

import asyncio
from dataclasses import dataclass, replace
from datetime import timedelta
import logging
from time import monotonic
from typing import Mapping

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST
from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .availability import SnapshotAvailabilityGuard
from .api import NamoApiClient, NamoApiError, SiteLeaderResponse
from .const import (
    CONF_SITE_ID,
    DIRECTORY_REFRESH_SECONDS,
    DOMAIN,
    MAX_PARALLEL_REQUESTS,
    POLL_INTERVAL_SECONDS,
    SPACE_METADATA_REFRESH_SECONDS,
    TRANSIENT_FAILURE_GRACE_SECONDS,
)
from .routing import (
    DirectorySnapshot,
    FloorplanSpaces,
    GroupRoute,
    management_candidate_urls,
    RouteContractError,
    SnapshotFence,
    SiteSnapshot,
    SpaceSnapshot,
    parse_automation_state,
    parse_directory,
    parse_floorplan_spaces,
    parse_standalone_state,
    resolve_site_routes,
    summarize_site,
    unavailable_snapshot,
)

_LOGGER = logging.getLogger(__name__)


@dataclass(frozen=True, slots=True)
class NamoCoordinatorData:
    site_id: str
    spaces: Mapping[str, SpaceSnapshot]
    site: SiteSnapshot


class NamoCoordinator(DataUpdateCoordinator[NamoCoordinatorData]):
    def __init__(self, hass: HomeAssistant, entry: ConfigEntry) -> None:
        super().__init__(
            hass,
            logger=_LOGGER,
            config_entry=entry,
            name=DOMAIN,
            update_interval=timedelta(seconds=POLL_INTERVAL_SECONDS),
            always_update=False,
        )
        self.entry = entry
        self._client = NamoApiClient(async_get_clientsession(hass))
        self._seed_url = entry.data[CONF_HOST]
        self._site_id = entry.data[CONF_SITE_ID]
        self._leader: SiteLeaderResponse | None = None
        self._directory: DirectorySnapshot | None = None
        self._routes: dict[str, GroupRoute] = {}
        self._participant_urls: tuple[str, ...] = ()
        self._snapshots: dict[str, SpaceSnapshot] = {}
        self._fence = SnapshotFence()
        self._next_directory_refresh = 0.0
        self._floorplan = FloorplanSpaces({}, None)
        self._next_space_metadata_refresh = 0.0
        self._route_issue_reasons: dict[str, str] = {}
        self._availability = SnapshotAvailabilityGuard(
            TRANSIENT_FAILURE_GRACE_SECONDS
        )

    def _record_route_status(
        self,
        route: GroupRoute,
        reason: str | None,
        detail: Exception | None = None,
        *,
        preserved: bool = False,
    ) -> None:
        previous = self._route_issue_reasons.get(route.space_id)
        if reason is None:
            if previous is not None:
                self._route_issue_reasons.pop(route.space_id, None)
                _LOGGER.info(
                    "NAMO Space %s recovered after %s",
                    route.space_id,
                    previous.removeprefix("preserved:"),
                )
            return
        status = f"preserved:{reason}" if preserved else reason
        if previous == status:
            return
        self._route_issue_reasons[route.space_id] = status
        if preserved:
            if detail is None:
                _LOGGER.warning(
                    "NAMO Space %s temporarily lost its source: %s; "
                    "preserving the last state for %ss",
                    route.space_id,
                    reason,
                    TRANSIENT_FAILURE_GRACE_SECONDS,
                )
            else:
                _LOGGER.warning(
                    "NAMO Space %s temporarily lost its source: %s (%s); "
                    "preserving the last state for %ss",
                    route.space_id,
                    reason,
                    detail,
                    TRANSIENT_FAILURE_GRACE_SECONDS,
                )
            return
        if detail is None:
            _LOGGER.warning(
                "NAMO Space %s is unavailable: %s",
                route.space_id,
                reason,
            )
        else:
            _LOGGER.warning(
                "NAMO Space %s is unavailable: %s (%s)",
                route.space_id,
                reason,
                detail,
            )

    def _candidate_seed_urls(self) -> tuple[str, ...]:
        return management_candidate_urls(
            self._seed_url,
            self._leader.base_url if self._leader is not None else None,
            tuple(route.base_url for route in self._routes.values()),
            self._participant_urls,
        )

    async def _async_refresh_directory(self) -> None:
        last_error: Exception | None = None
        leader: SiteLeaderResponse | None = None
        for candidate in self._candidate_seed_urls():
            try:
                leader = await self._client.async_resolve_site_leader(candidate)
                break
            except (NamoApiError, RouteContractError) as err:
                last_error = err
        if leader is None:
            raise UpdateFailed("NAMO management device is unavailable") from last_error
        if leader.site_id != self._site_id:
            raise UpdateFailed("NAMO device belongs to a different installation")
        try:
            participant_urls = await self._client.async_get_participant_urls(
                leader.base_url
            )
        except NamoApiError:
            participant_urls = ()
        if participant_urls:
            self._participant_urls = participant_urls
        if monotonic() >= self._next_space_metadata_refresh:
            try:
                floorplan_payload = await self._client.async_get_json(
                    leader.base_url, "/api/floorplan"
                )
                self._floorplan = parse_floorplan_spaces(floorplan_payload)
            except NamoApiError:
                pass
            self._next_space_metadata_refresh = (
                monotonic() + SPACE_METADATA_REFRESH_SECONDS
            )
        try:
            directory = parse_directory(
                leader_url=leader.base_url,
                site_id=leader.site_id,
                groups_payload=leader.groups,
            )
        except RouteContractError as err:
            raise UpdateFailed("NAMO route directory is invalid") from err
        routes = resolve_site_routes(
            directory,
            mode=leader.mode,
            node_id=leader.node_id,
            base_url=leader.base_url,
            device_name=leader.device_name,
            floorplan=self._floorplan,
        )
        directory = replace(directory, routes=routes)
        if self.entry.title == "NAMO":
            title = routes[0].display_name if len(routes) == 1 else leader.device_name
            self.hass.config_entries.async_update_entry(
                self.entry, title=f"NAMO {title}"
            )
        self._leader = leader
        self._directory = directory
        self._routes = {route.space_id: route for route in directory.routes}
        self._next_directory_refresh = monotonic() + DIRECTORY_REFRESH_SECONDS

    async def _async_read_route(
        self, route: GroupRoute, semaphore: asyncio.Semaphore
    ) -> SpaceSnapshot:
        previous = self._snapshots.get(route.space_id) or self._fence.previous(
            route.space_id
        )
        if not route.available or route.base_url is None:
            snapshot, preserved = self._availability.reject(
                route,
                previous,
                reason="route_unavailable",
                now=monotonic(),
            )
            self._record_route_status(
                route, "route_unavailable", preserved=preserved
            )
            return snapshot
        try:
            async with semaphore:
                payload = await self._client.async_get_json(
                    route.base_url, route.state_path
                )
            if route.standalone:
                snapshot = parse_standalone_state(route, payload, previous)
            else:
                snapshot = parse_automation_state(route, payload, previous)
                if not snapshot.continuity_held:
                    snapshot = self._fence.accept(snapshot)
            if not snapshot.continuity_held:
                self._availability.accept(snapshot)
            self._record_route_status(
                route,
                snapshot.unavailable_reason,
                preserved=snapshot.continuity_held,
            )
            return snapshot
        except NamoApiError as err:
            snapshot, preserved = self._availability.reject(
                route,
                previous,
                reason="request_failed",
                now=monotonic(),
            )
            self._record_route_status(
                route,
                "request_failed",
                err,
                preserved=preserved,
            )
            return snapshot
        except RouteContractError as err:
            self._availability.discard(route.space_id)
            self._record_route_status(route, "invalid_snapshot", err)
            return unavailable_snapshot(
                route, previous, reason="invalid_snapshot"
            )

    async def _async_update_data(self) -> NamoCoordinatorData:
        directory_due = monotonic() >= self._next_directory_refresh
        if directory_due or self._directory is None:
            try:
                await self._async_refresh_directory()
            except UpdateFailed:
                if self._directory is None:
                    raise
                self._next_directory_refresh = monotonic() + 2

        semaphore = asyncio.Semaphore(MAX_PARALLEL_REQUESTS)
        current = await asyncio.gather(
            *(
                self._async_read_route(route, semaphore)
                for route in self._routes.values()
            )
        )
        next_snapshots = {item.space_id: item for item in current}
        for space_id, previous in self._snapshots.items():
            if space_id in next_snapshots:
                continue
            old_route = GroupRoute(
                site_id=previous.site_id,
                space_id=previous.space_id,
                group_id=previous.group_id,
                leader_node_id=previous.leader_node_id,
                group_term=previous.group_term,
                base_url=None,
                state_path="/api/state",
                available=False,
                display_name=previous.display_name,
                standalone=previous.standalone,
                participants_online=0,
                participants_expected=previous.participants_expected,
                group_health="unavailable",
            )
            self._record_route_status(old_route, "route_removed")
            self._availability.discard(space_id)
            next_snapshots[space_id] = unavailable_snapshot(
                old_route, previous, reason="route_removed"
            )
        self._availability.retain(set(next_snapshots))
        self._snapshots = next_snapshots
        return NamoCoordinatorData(
            self._site_id,
            dict(next_snapshots),
            summarize_site(self._site_id, next_snapshots),
        )

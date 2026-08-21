from __future__ import annotations

from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN
from .coordinator import NamoCoordinator
from .entity_model import (
    space_device_identifier,
    space_device_name,
)
from .routing import SpaceSnapshot


class NamoSpaceEntity(CoordinatorEntity[NamoCoordinator]):
    _attr_has_entity_name = True

    def __init__(self, coordinator: NamoCoordinator, space_id: str) -> None:
        super().__init__(coordinator)
        self.space_id = space_id

    @property
    def snapshot(self) -> SpaceSnapshot | None:
        data = self.coordinator.data
        return None if data is None else data.spaces.get(self.space_id)

    @property
    def available(self) -> bool:
        snapshot = self.snapshot
        return (
            super().available
            and snapshot is not None
            and snapshot.available
        )

    @property
    def device_info(self) -> DeviceInfo:
        site_id = self.coordinator.entry.data["site_id"]
        return DeviceInfo(
            identifiers={(
                DOMAIN,
                space_device_identifier(site_id, self.space_id),
            )},
            manufacturer="NAMO",
            model="Spatial Presence",
            name=space_device_name(self.snapshot, self.space_id),
        )

from __future__ import annotations

from homeassistant.components.binary_sensor import (
    BinarySensorDeviceClass,
    BinarySensorEntity,
)
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import NamoConfigEntry
from .coordinator import NamoCoordinator
from .entity import NamoSpaceEntity
from .entity_model import area_entity_unique_id, space_entity_unique_id


class NamoOccupancyBinarySensor(NamoSpaceEntity, BinarySensorEntity):
    _attr_device_class = BinarySensorDeviceClass.OCCUPANCY
    _attr_translation_key = "occupancy"

    def __init__(self, coordinator: NamoCoordinator, space_id: str) -> None:
        super().__init__(coordinator, space_id)
        self._attr_unique_id = space_entity_unique_id(
            coordinator.entry.data["site_id"], space_id, "occupancy"
        )

    @property
    def is_on(self) -> bool | None:
        snapshot = self.snapshot
        return None if snapshot is None else snapshot.presence


class NamoMotionBinarySensor(NamoSpaceEntity, BinarySensorEntity):
    _attr_device_class = BinarySensorDeviceClass.MOTION
    _attr_translation_key = "motion"

    def __init__(self, coordinator: NamoCoordinator, space_id: str) -> None:
        super().__init__(coordinator, space_id)
        self._attr_unique_id = space_entity_unique_id(
            coordinator.entry.data["site_id"], space_id, "motion"
        )

    @property
    def is_on(self) -> bool | None:
        snapshot = self.snapshot
        return None if snapshot is None else snapshot.motion


class NamoAreaOccupancyBinarySensor(NamoSpaceEntity, BinarySensorEntity):
    _attr_device_class = BinarySensorDeviceClass.OCCUPANCY

    def __init__(
        self,
        coordinator: NamoCoordinator,
        space_id: str,
        floorplan_id: str,
        area_id: str,
    ) -> None:
        super().__init__(coordinator, space_id)
        self.floorplan_id = floorplan_id
        self.area_id = area_id
        self._attr_unique_id = area_entity_unique_id(
            coordinator.entry.data["site_id"],
            floorplan_id,
            area_id,
            "occupancy",
        )

    @property
    def area(self):
        snapshot = self.snapshot
        if snapshot is None or snapshot.floorplan_id != self.floorplan_id:
            return None
        return next(
            (area for area in snapshot.areas if area.area_id == self.area_id),
            None,
        )

    @property
    def name(self) -> str:
        area = self.area
        return self.area_id if area is None else area.name

    @property
    def available(self) -> bool:
        area = self.area
        return super().available and area is not None and area.presence is not None

    @property
    def is_on(self) -> bool | None:
        area = self.area
        return None if area is None else area.presence


async def async_setup_entry(
    hass: HomeAssistant,
    entry: NamoConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    coordinator = entry.runtime_data
    known: set[str] = set()

    def add_new_entities() -> None:
        data = coordinator.data
        if data is None:
            return
        entities: list[BinarySensorEntity] = []
        for space_id in data.spaces:
            for suffix, factory in (
                ("occupancy", NamoOccupancyBinarySensor),
                ("motion", NamoMotionBinarySensor),
            ):
                key = f"{space_id}:{suffix}"
                if key not in known:
                    known.add(key)
                    entities.append(factory(coordinator, space_id))
            snapshot = data.spaces[space_id]
            if snapshot.floorplan_id is None:
                continue
            for area in snapshot.areas:
                key = (
                    f"{space_id}:{snapshot.floorplan_id}:"
                    f"{area.area_id}:occupancy"
                )
                if key in known:
                    continue
                known.add(key)
                entities.append(
                    NamoAreaOccupancyBinarySensor(
                        coordinator,
                        space_id,
                        snapshot.floorplan_id,
                        area.area_id,
                    )
                )
        if entities:
            async_add_entities(entities)

    add_new_entities()
    entry.async_on_unload(coordinator.async_add_listener(add_new_entities))

from __future__ import annotations

from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorStateClass,
)
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import NamoConfigEntry
from .coordinator import NamoCoordinator
from .entity import NamoSpaceEntity
from .entity_model import space_entity_unique_id


class NamoTargetCountSensor(NamoSpaceEntity, SensorEntity):
    _attr_translation_key = "target_count"
    _attr_native_unit_of_measurement = "people"
    _attr_state_class = SensorStateClass.MEASUREMENT

    def __init__(self, coordinator: NamoCoordinator, space_id: str) -> None:
        super().__init__(coordinator, space_id)
        self._attr_unique_id = space_entity_unique_id(
            coordinator.entry.data["site_id"], space_id, "target_count"
        )

    @property
    def native_value(self) -> int | None:
        snapshot = self.snapshot
        return None if snapshot is None else snapshot.target_count


class NamoHealthSensor(NamoSpaceEntity, SensorEntity):
    _attr_translation_key = "health"
    _attr_device_class = SensorDeviceClass.ENUM
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _attr_options = ["healthy", "degraded", "unavailable"]

    def __init__(self, coordinator: NamoCoordinator, space_id: str) -> None:
        super().__init__(coordinator, space_id)
        self._attr_unique_id = space_entity_unique_id(
            coordinator.entry.data["site_id"], space_id, "health"
        )

    @property
    def available(self) -> bool:
        return self.coordinator.last_update_success and self.snapshot is not None

    @property
    def native_value(self) -> str | None:
        snapshot = self.snapshot
        return None if snapshot is None else snapshot.health

    @property
    def extra_state_attributes(self) -> dict[str, str | int] | None:
        snapshot = self.snapshot
        if snapshot is None or snapshot.unavailable_reason is None:
            return None
        return {"reason": snapshot.unavailable_reason}


class NamoDetectionModeSensor(NamoSpaceEntity, SensorEntity):
    _attr_translation_key = "detection_mode"
    _attr_device_class = SensorDeviceClass.ENUM
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _attr_options = ["standalone", "single_sensor", "multi_sensor"]

    def __init__(self, coordinator: NamoCoordinator, space_id: str) -> None:
        super().__init__(coordinator, space_id)
        self._attr_unique_id = space_entity_unique_id(
            coordinator.entry.data["site_id"], space_id, "detection_mode"
        )

    @property
    def native_value(self) -> str | None:
        snapshot = self.snapshot
        if snapshot is None:
            return None
        if snapshot.standalone:
            return "standalone"
        return (
            "multi_sensor"
            if snapshot.participants_expected > 1
            else "single_sensor"
        )


class NamoFusionStatusSensor(NamoSpaceEntity, SensorEntity):
    _attr_translation_key = "fusion_status"
    _attr_device_class = SensorDeviceClass.ENUM
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _attr_options = [
        "not_used",
        "single_sensor",
        "active",
        "limited",
        "unavailable",
    ]

    def __init__(self, coordinator: NamoCoordinator, space_id: str) -> None:
        super().__init__(coordinator, space_id)
        self._attr_unique_id = space_entity_unique_id(
            coordinator.entry.data["site_id"], space_id, "fusion_status"
        )

    @property
    def available(self) -> bool:
        return self.coordinator.last_update_success and self.snapshot is not None

    @property
    def native_value(self) -> str | None:
        snapshot = self.snapshot
        if snapshot is None:
            return None
        if snapshot.standalone:
            return "not_used"
        if snapshot.participants_expected <= 1:
            return "single_sensor"
        if not snapshot.available:
            return "unavailable"
        if (
            snapshot.health == "healthy"
            and snapshot.participants_online == snapshot.participants_expected
        ):
            return "active"
        return "limited"


class NamoDevicesOnlineSensor(NamoSpaceEntity, SensorEntity):
    _attr_translation_key = "devices_online"
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _attr_state_class = SensorStateClass.MEASUREMENT

    def __init__(self, coordinator: NamoCoordinator, space_id: str) -> None:
        super().__init__(coordinator, space_id)
        self._attr_unique_id = space_entity_unique_id(
            coordinator.entry.data["site_id"], space_id, "devices_online"
        )

    @property
    def native_value(self) -> int | None:
        snapshot = self.snapshot
        return None if snapshot is None else snapshot.participants_online


class NamoDevicesExpectedSensor(NamoSpaceEntity, SensorEntity):
    _attr_translation_key = "devices_expected"
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _attr_state_class = SensorStateClass.MEASUREMENT

    def __init__(self, coordinator: NamoCoordinator, space_id: str) -> None:
        super().__init__(coordinator, space_id)
        self._attr_unique_id = space_entity_unique_id(
            coordinator.entry.data["site_id"], space_id, "devices_expected"
        )

    @property
    def native_value(self) -> int | None:
        snapshot = self.snapshot
        return None if snapshot is None else snapshot.participants_expected


class NamoDataAgeSensor(NamoSpaceEntity, SensorEntity):
    _attr_translation_key = "data_age"
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _attr_native_unit_of_measurement = "ms"
    _attr_state_class = SensorStateClass.MEASUREMENT

    def __init__(self, coordinator: NamoCoordinator, space_id: str) -> None:
        super().__init__(coordinator, space_id)
        self._attr_unique_id = space_entity_unique_id(
            coordinator.entry.data["site_id"], space_id, "data_age"
        )

    @property
    def native_value(self) -> int | None:
        snapshot = self.snapshot
        return None if snapshot is None else snapshot.source_age_ms


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
        entities: list[SensorEntity] = []
        for space_id in data.spaces:
            if space_id in known:
                continue
            known.add(space_id)
            entities.extend(
                (
                    NamoTargetCountSensor(coordinator, space_id),
                    NamoHealthSensor(coordinator, space_id),
                    NamoDetectionModeSensor(coordinator, space_id),
                    NamoFusionStatusSensor(coordinator, space_id),
                    NamoDevicesOnlineSensor(coordinator, space_id),
                    NamoDevicesExpectedSensor(coordinator, space_id),
                    NamoDataAgeSensor(coordinator, space_id),
                )
            )
        if entities:
            async_add_entities(entities)

    add_new_entities()
    entry.async_on_unload(coordinator.async_add_listener(add_new_entities))

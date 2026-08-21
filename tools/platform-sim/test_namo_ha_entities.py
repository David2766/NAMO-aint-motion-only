from __future__ import annotations

import asyncio
import importlib.util
from pathlib import Path
import sys
import types
import unittest


ROOT = Path(__file__).resolve().parents[2]
PACKAGE_ROOT = ROOT / "custom_components" / "namo"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def install_home_assistant_stubs() -> None:
    homeassistant = types.ModuleType("homeassistant")
    components = types.ModuleType("homeassistant.components")
    binary_sensor = types.ModuleType("homeassistant.components.binary_sensor")
    sensor = types.ModuleType("homeassistant.components.sensor")
    config_entries = types.ModuleType("homeassistant.config_entries")
    core = types.ModuleType("homeassistant.core")
    const = types.ModuleType("homeassistant.const")
    helpers = types.ModuleType("homeassistant.helpers")
    device_registry = types.ModuleType("homeassistant.helpers.device_registry")
    entity_platform = types.ModuleType("homeassistant.helpers.entity_platform")
    entity_registry = types.ModuleType("homeassistant.helpers.entity_registry")
    issue_registry = types.ModuleType("homeassistant.helpers.issue_registry")
    update_coordinator = types.ModuleType(
        "homeassistant.helpers.update_coordinator"
    )

    class CoordinatorEntity:
        def __class_getitem__(cls, _item):
            return cls

        def __init__(self, coordinator):
            self.coordinator = coordinator

        @property
        def available(self):
            return True

    class DeviceInfo(dict):
        def __init__(self, **values):
            super().__init__(values)

    class BinarySensorEntity:
        pass

    class BinarySensorDeviceClass:
        OCCUPANCY = "occupancy"
        MOTION = "motion"

    class SensorEntity:
        pass

    class SensorDeviceClass:
        ENUM = "enum"

    class SensorStateClass:
        MEASUREMENT = "measurement"

    class EntityCategory:
        DIAGNOSTIC = "diagnostic"

    class IssueSeverity:
        WARNING = "warning"

    class ConfigEntry:
        def __class_getitem__(cls, _item):
            return cls

    binary_sensor.BinarySensorEntity = BinarySensorEntity
    binary_sensor.BinarySensorDeviceClass = BinarySensorDeviceClass
    sensor.SensorEntity = SensorEntity
    sensor.SensorDeviceClass = SensorDeviceClass
    sensor.SensorStateClass = SensorStateClass
    config_entries.ConfigEntry = ConfigEntry
    core.HomeAssistant = object
    const.EntityCategory = EntityCategory
    device_registry.DeviceInfo = DeviceInfo
    entity_platform.AddConfigEntryEntitiesCallback = object
    helpers.device_registry = device_registry
    helpers.entity_registry = entity_registry
    helpers.issue_registry = issue_registry
    issue_registry.IssueSeverity = IssueSeverity
    issue_registry.async_create_issue = lambda *_args, **_kwargs: None
    issue_registry.async_delete_issue = lambda *_args, **_kwargs: None
    update_coordinator.CoordinatorEntity = CoordinatorEntity

    for name, module in (
        ("homeassistant", homeassistant),
        ("homeassistant.components", components),
        ("homeassistant.components.binary_sensor", binary_sensor),
        ("homeassistant.components.sensor", sensor),
        ("homeassistant.config_entries", config_entries),
        ("homeassistant.core", core),
        ("homeassistant.const", const),
        ("homeassistant.helpers", helpers),
        ("homeassistant.helpers.device_registry", device_registry),
        ("homeassistant.helpers.entity_platform", entity_platform),
        ("homeassistant.helpers.entity_registry", entity_registry),
        ("homeassistant.helpers.issue_registry", issue_registry),
        ("homeassistant.helpers.update_coordinator", update_coordinator),
    ):
        sys.modules[name] = module


install_home_assistant_stubs()
package = types.ModuleType("custom_components.namo")
package.__path__ = [str(PACKAGE_ROOT)]
package.NamoConfigEntry = object
sys.modules.setdefault("custom_components", types.ModuleType("custom_components"))
sys.modules["custom_components.namo"] = package
load_module("custom_components.namo.const", PACKAGE_ROOT / "const.py")
routing = load_module("custom_components.namo.routing", PACKAGE_ROOT / "routing.py")
load_module(
    "custom_components.namo.entity_model", PACKAGE_ROOT / "entity_model.py"
)
coordinator_module = types.ModuleType("custom_components.namo.coordinator")
coordinator_module.NamoCoordinator = object
sys.modules["custom_components.namo.coordinator"] = coordinator_module
integration = load_module(
    "custom_components.namo", PACKAGE_ROOT / "__init__.py"
)
repairs = sys.modules["custom_components.namo.repairs"]
entity = load_module("custom_components.namo.entity", PACKAGE_ROOT / "entity.py")
binary_sensor = load_module(
    "custom_components.namo.binary_sensor", PACKAGE_ROOT / "binary_sensor.py"
)
sensor = load_module("custom_components.namo.sensor", PACKAGE_ROOT / "sensor.py")


def snapshot(space_id: str, name: str, area_id: str):
    return routing.SpaceSnapshot(
        site_id="site-main",
        space_id=space_id,
        group_id=f"group-{space_id}",
        leader_node_id=f"leader-{space_id}",
        group_term=1,
        available=True,
        health="healthy",
        presence=True,
        motion=False,
        target_count=1,
        boot_id="boot-a",
        sequence=1,
        source_sequence=1,
        floorplan_id="floorplan-main",
        areas_revision=3,
        areas_available=True,
        areas=(
            routing.AreaSnapshot(
                area_id=area_id,
                name=f"Area {area_id}",
                presence=True,
                target_count=1,
            ),
        ),
        display_name=name,
        participants_online=2,
        participants_expected=2,
        source_age_ms=80,
    )


class FakeCoordinator:
    def __init__(self):
        self.entry = types.SimpleNamespace(
            data={"site_id": "site-main"},
            title="NAMO Home",
        )
        spaces = {
                "space-living": snapshot(
                    "space-living", "Living room", "zone_1"
                ),
                "space-hall": snapshot("space-hall", "Hall", "zone_2"),
        }
        self.data = types.SimpleNamespace(
            spaces=spaces,
            site=routing.summarize_site("site-main", spaces),
        )
        self.last_update_success = True
        self.listeners = []

    def async_add_listener(self, listener):
        self.listeners.append(listener)
        return lambda: None


class NamoHaEntityTests(unittest.TestCase):
    def setUp(self):
        self.coordinator = FakeCoordinator()

    def test_multiple_spaces_create_separate_devices_and_entities(self):
        binary_entities = []
        sensor_entities = []
        entry = types.SimpleNamespace(
            runtime_data=self.coordinator,
            async_on_unload=lambda _callback: None,
        )
        asyncio.run(
            binary_sensor.async_setup_entry(
                None, entry, binary_entities.extend
            )
        )
        asyncio.run(
            sensor.async_setup_entry(None, entry, sensor_entities.extend)
        )

        self.assertEqual(len(binary_entities), 6)
        self.assertEqual(len(sensor_entities), 14)
        identifiers = {
            next(iter(item.device_info["identifiers"]))[1]
            for item in (*binary_entities, *sensor_entities)
        }
        self.assertEqual(
            identifiers,
            {
                "site-main:space-living",
                "site-main:space-hall",
            },
        )
        self.assertTrue(
            all("via_device" not in item.device_info for item in sensor_entities)
        )

    def test_shared_area_is_exposed_under_its_space_device(self):
        area = binary_sensor.NamoAreaOccupancyBinarySensor(
            self.coordinator,
            "space-living",
            "floorplan-main",
            "zone_1",
        )
        self.assertEqual(area.name, "Area zone_1")
        self.assertTrue(area.is_on)
        self.assertEqual(
            area._attr_unique_id,
            "site-main:floorplan-main:area:zone_1:occupancy",
        )
        self.assertEqual(
            next(iter(area.device_info["identifiers"]))[1],
            "site-main:space-living",
        )

    def test_space_names_are_user_facing_while_ids_remain_stable(self):
        occupancy = binary_sensor.NamoOccupancyBinarySensor(
            self.coordinator, "space-living"
        )
        self.assertEqual(occupancy.device_info["name"], "NAMO Living room")
        self.assertEqual(
            occupancy._attr_unique_id,
            "site-main:space-living:occupancy",
        )

    def test_new_space_adds_entities_once_without_reloading_site(self):
        binary_entities = []
        sensor_entities = []
        entry = types.SimpleNamespace(
            runtime_data=self.coordinator,
            async_on_unload=lambda _callback: None,
        )
        asyncio.run(
            binary_sensor.async_setup_entry(None, entry, binary_entities.extend)
        )
        asyncio.run(sensor.async_setup_entry(None, entry, sensor_entities.extend))
        self.coordinator.data.spaces["space-office"] = snapshot(
            "space-office", "Office", "zone_3"
        )
        self.coordinator.data.site = routing.summarize_site(
            "site-main", self.coordinator.data.spaces
        )
        for listener in self.coordinator.listeners:
            listener()
        self.assertEqual(len(binary_entities), 9)
        self.assertEqual(len(sensor_entities), 21)
        for listener in self.coordinator.listeners:
            listener()
        self.assertEqual(len(binary_entities), 9)
        self.assertEqual(len(sensor_entities), 21)

    def test_unavailable_space_is_not_reported_as_unoccupied(self):
        current = self.coordinator.data.spaces["space-living"]
        self.coordinator.data.spaces["space-living"] = routing.unavailable_snapshot(
            routing.GroupRoute(
                site_id=current.site_id,
                space_id=current.space_id,
                group_id=current.group_id,
                leader_node_id=current.leader_node_id,
                group_term=current.group_term,
                base_url=None,
                state_path="/api/state",
                available=False,
                display_name=current.display_name,
                participants_online=current.participants_online,
                participants_expected=current.participants_expected,
                group_health=current.health,
            ),
            current,
        )
        occupancy = binary_sensor.NamoOccupancyBinarySensor(
            self.coordinator, "space-living"
        )
        self.assertFalse(occupancy.available)
        self.assertIsNone(occupancy.is_on)
        health = sensor.NamoHealthSensor(self.coordinator, "space-living")
        self.assertEqual(
            health.extra_state_attributes,
            {"reason": "unavailable"},
        )

    def test_group_diagnostics_are_visible_entities(self):
        health = sensor.NamoHealthSensor(self.coordinator, "space-living")
        self.assertIsNone(health.extra_state_attributes)
        self.assertEqual(
            sensor.NamoDetectionModeSensor(
                self.coordinator, "space-living"
            ).native_value,
            "multi_sensor",
        )
        self.assertEqual(
            sensor.NamoFusionStatusSensor(
                self.coordinator, "space-living"
            ).native_value,
            "active",
        )
        self.assertEqual(
            sensor.NamoDevicesOnlineSensor(
                self.coordinator, "space-living"
            ).native_value,
            2,
        )
        self.assertEqual(
            sensor.NamoDevicesExpectedSensor(
                self.coordinator, "space-living"
            ).native_value,
            2,
        )
        self.assertEqual(
            sensor.NamoDataAgeSensor(
                self.coordinator, "space-living"
            ).native_value,
            80,
        )

    def test_repairs_issue_waits_for_persistent_problem_and_recovers(self):
        current = self.coordinator.data.spaces["space-living"]
        self.coordinator.data.spaces["space-living"] = routing.replace(
            current,
            health="degraded",
            participants_online=1,
        )
        self.coordinator.data.site = routing.summarize_site(
            "site-main", self.coordinator.data.spaces
        )
        entry = types.SimpleNamespace(
            entry_id="entry-main",
            title="NAMO Home",
            runtime_data=self.coordinator,
        )
        created = []
        deleted = []
        repairs.ir.async_create_issue = (
            lambda *_args, **kwargs: created.append(kwargs)
        )
        repairs.ir.async_delete_issue = (
            lambda *_args, **_kwargs: deleted.append(True)
        )
        now = [100.0]
        repairs.monotonic = lambda: now[0]
        tracker = repairs.NamoIssueTracker(object(), entry)
        tracker()
        self.assertEqual(created, [])
        now[0] += 31
        tracker()
        self.assertEqual(len(created), 1)
        self.assertEqual(
            created[0]["translation_placeholders"]["spaces"],
            "Living room",
        )
        self.coordinator.data.spaces["space-living"] = current
        self.coordinator.data.site = routing.summarize_site(
            "site-main", self.coordinator.data.spaces
        )
        tracker()
        self.assertEqual(deleted, [True])

    def test_version_three_migration_removes_legacy_site_device(self):
        entries = [
            types.SimpleNamespace(
                entity_id="binary_sensor.living_zone_1",
                unique_id="site-main:space-living:zone:zone_1:occupancy",
            ),
            types.SimpleNamespace(
                entity_id="binary_sensor.living_occupancy",
                unique_id="site-main:space-living:occupancy",
            ),
            types.SimpleNamespace(
                entity_id="sensor.namo_overall_status",
                unique_id="site-main:site:status",
            ),
        ]

        class Registry:
            def __init__(self):
                self.removed = []

            def async_remove(self, entity_id):
                self.removed.append(entity_id)

        registry = Registry()
        integration.er.async_get = lambda _hass: registry
        integration.er.async_entries_for_config_entry = (
            lambda _registry, _entry_id: entries
        )
        site_device = types.SimpleNamespace(
            id="site-device", via_device_id=None
        )
        space_device = types.SimpleNamespace(
            id="space-device", via_device_id="site-device"
        )

        class DeviceRegistry:
            def __init__(self):
                self.updated = []
                self.removed = []

            def async_get_device(self, *, identifiers):
                self.identifiers = identifiers
                return site_device

            def async_update_device(self, device_id, **values):
                self.updated.append((device_id, values))

            def async_remove_device(self, device_id):
                self.removed.append(device_id)

        device_registry = DeviceRegistry()
        integration.dr.async_get = lambda _hass: device_registry
        integration.dr.async_entries_for_config_entry = (
            lambda _registry, _entry_id: [site_device, space_device]
        )
        updated = {}
        entry = types.SimpleNamespace(
            version=1,
            entry_id="entry-main",
            data={"site_id": "site-main"},
        )

        def update_entry(target, **values):
            updated.update(values)
            if "version" in values:
                target.version = values["version"]

        hass = types.SimpleNamespace(
            config_entries=types.SimpleNamespace(
                async_update_entry=update_entry
            )
        )
        self.assertTrue(asyncio.run(integration.async_migrate_entry(hass, entry)))
        self.assertEqual(
            registry.removed,
            [
                "binary_sensor.living_zone_1",
                "sensor.namo_overall_status",
            ],
        )
        self.assertEqual(
            device_registry.identifiers,
            {("namo", "site:site-main")},
        )
        self.assertEqual(
            device_registry.updated,
            [("space-device", {"via_device_id": None})],
        )
        self.assertEqual(device_registry.removed, ["site-device"])
        self.assertEqual(updated, {"version": 3})


if __name__ == "__main__":
    unittest.main()

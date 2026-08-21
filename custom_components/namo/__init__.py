from __future__ import annotations

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers import device_registry as dr, entity_registry as er

from .const import CONF_SITE_ID, DOMAIN, PLATFORMS
from .coordinator import NamoCoordinator
from .entity_model import (
    is_legacy_zone_unique_id,
    legacy_site_status_unique_id,
    site_device_identifier,
)
from .repairs import NamoIssueTracker, delete_site_issue

NamoConfigEntry = ConfigEntry[NamoCoordinator]


async def async_migrate_entry(
    hass: HomeAssistant, entry: NamoConfigEntry
) -> bool:
    if entry.version > 3:
        return False
    registry = er.async_get(hass)
    if entry.version < 2:
        for registry_entry in er.async_entries_for_config_entry(
            registry, entry.entry_id
        ):
            if is_legacy_zone_unique_id(registry_entry.unique_id):
                registry.async_remove(registry_entry.entity_id)
        hass.config_entries.async_update_entry(entry, version=2)
    if entry.version < 3:
        site_id = entry.data.get(CONF_SITE_ID)
        if isinstance(site_id, str) and site_id:
            site_status_id = legacy_site_status_unique_id(site_id)
            for registry_entry in er.async_entries_for_config_entry(
                registry, entry.entry_id
            ):
                if registry_entry.unique_id == site_status_id:
                    registry.async_remove(registry_entry.entity_id)

            device_registry = dr.async_get(hass)
            site_device = device_registry.async_get_device(
                identifiers={(DOMAIN, site_device_identifier(site_id))}
            )
            if site_device is not None:
                for device in dr.async_entries_for_config_entry(
                    device_registry, entry.entry_id
                ):
                    if device.via_device_id == site_device.id:
                        device_registry.async_update_device(
                            device.id, via_device_id=None
                        )
                device_registry.async_remove_device(site_device.id)
        hass.config_entries.async_update_entry(entry, version=3)
    return True


async def async_setup_entry(hass: HomeAssistant, entry: NamoConfigEntry) -> bool:
    coordinator = NamoCoordinator(hass, entry)
    await coordinator.async_config_entry_first_refresh()
    entry.runtime_data = coordinator
    issue_tracker = NamoIssueTracker(hass, entry)
    issue_tracker()
    entry.async_on_unload(coordinator.async_add_listener(issue_tracker))
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: NamoConfigEntry) -> bool:
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)


async def async_remove_entry(hass: HomeAssistant, entry: NamoConfigEntry) -> None:
    delete_site_issue(hass, entry)

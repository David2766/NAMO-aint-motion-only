# Home Assistant Integration

Status: development preview. The integration is not release-verified yet.

## Install with HACS

The integration is published separately from the firmware repository. Until it
is included in the HACS default list, add
`https://github.com/David2766/NAMO-Home-Assistant` as a custom **Integration**
repository, install NAMO, and restart Home Assistant.

Open **Settings > Devices & services > Add integration**, select NAMO, and
confirm the discovered Site. If discovery does not appear, enter the address of
any NAMO device already connected to that Site.

## Install manually

1. Copy `custom_components/namo` into the Home Assistant configuration
   directory under `custom_components/namo`.
2. Restart Home Assistant.
3. Open **Settings > Devices & services > Add integration** and select NAMO.
4. If discovery does not appear, enter the address of any NAMO device already
   connected to that Site.

The config entry is keyed by the stable Site identity, not an IP address.
Discovery can therefore update a changed address without creating a second
entry. Physical Nodes in the same Site do not create duplicate entries.

## Home Assistant model

- One NAMO config entry represents one Site.
- Each logical Space below that Site is one Home Assistant device.
- A Site with several Groups exposes several Space devices through the same
  config entry.
- A one-Node standalone Site exposes one local Space even before a Group is
  configured.
- Current Site and Group Leader identities are routing details and do not
  appear in entity unique IDs.
- New Site entries use the discovered device name. An existing entry still
  carrying the default `NAMO` title is labeled once from its single Space name
  when available; user-edited titles are preserved.

For example, one standalone Node and two fused Nodes form two Sites. Home
Assistant stores two NAMO config entries. The standalone Site exposes one Space
device and the fused Site exposes one device for each configured Space.

## State model

- Home Assistant resolves the current management device at a low rate.
- It reads the bounded Space route directory from that device.
- It then polls each Space's current calculation device directly.
- A timeout, stale authority, or route change makes entities unavailable. It
  never publishes an artificial unoccupied or zero state.
- Entity unique IDs use stable installation and Space identities and do not
  contain a device address or current Leader identity.

The integration creates Space occupancy, motion, target-count, and diagnostic
health entities. It also creates occupancy entities for enabled canonical
automation areas stored in the replicated Floorplan. Their unique IDs use the
stable Site, Floorplan, and area identities, so a Configuration Owner or Group
Leader handover does not recreate them. If the authoritative source is
unavailable, area entities become unavailable instead of reporting clear.

Device-local detection, reduced, excluded, and calibration Zones are
intentionally ignored by the integration. They remain private sensor filters
and are never merged into canonical Home Assistant areas. Existing ESPHome
physical entities remain independent.

Each Space device exposes status, detection mode, fusion status, online and
configured device counts, and source data age as visible diagnostic entities.
An unavailable reason remains attached to the Space status entity when needed.
The Site does not create a second parent device: Site-wide persistent problems
are reported through Home Assistant Repairs. A transient restart does not create
an alert; a non-healthy Site that persists for 30 seconds creates one Repairs
issue and clears it automatically after recovery.

Floorplan room names are used as device names when available. Stable Site and
Space IDs remain unchanged when a room is renamed.

Integration entry version 2 removes the misleading Zone occupancy entities
created by the development-preview version 1. Version 3 removes the obsolete
Site-status entity and its parent-only device, then detaches existing Space
devices from that parent. Space occupancy, motion, target count, and health
entity IDs remain unchanged so their history and automations are preserved.

## Local contract tests

Run the bounded route and entity-registration tests without a Home Assistant
installation:

```powershell
python -m unittest `
  tools.platform-sim.test_namo_ha_contract `
  tools.platform-sim.test_namo_ha_entities
```

These tests cover standalone fallback, multiple Spaces under one Site, stable
Space and canonical-area identity, Configuration Owner handover identity,
legacy Zone and Site-device cleanup, visible bounded Space diagnostics,
delayed Repairs reporting and recovery, unavailable reason reporting, dynamic
Space and area addition, translations, and package metadata. The firmware's
physical seven-Node handover path is verified separately; Home Assistant
container behavior during live handover, discovery, and DHCP changes remains a
release gate for this integration.

## Export the release repository

The development source remains in `custom_components/namo`. Export it to a
separate repository so Home Assistant Integration releases do not share tags or
versions with firmware releases.

```powershell
node scripts/export-home-assistant-integration.mjs `
  C:\path\to\NAMO-Home-Assistant
```

The exporter requires an explicit target, validates the complete staged
repository before writing it, rejects other integrations in the target, and
refuses a dirty Git target unless `--allow-dirty-target` is explicitly supplied.

from __future__ import annotations

import dataclasses
import importlib.util
import json
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


package = types.ModuleType("custom_components.namo")
package.__path__ = [str(PACKAGE_ROOT)]
sys.modules.setdefault("custom_components", types.ModuleType("custom_components"))
sys.modules["custom_components.namo"] = package
load_module("custom_components.namo.const", PACKAGE_ROOT / "const.py")
routing = load_module("custom_components.namo.routing", PACKAGE_ROOT / "routing.py")
availability = load_module(
    "custom_components.namo.availability", PACKAGE_ROOT / "availability.py"
)
entity_model = load_module(
    "custom_components.namo.entity_model", PACKAGE_ROOT / "entity_model.py"
)


def directory_payload(*, local: bool = False, address: str = "192.0.2.20"):
    return {
        "site": {
            "leaderNodeId": "node-site",
            "localLeader": True,
        },
        "siteGroupSummary": {
            "localAuthority": True,
            "capacity": 8,
            "configuredGroups": 1,
            "reportingGroups": 1,
        },
        "siteGroups": [{
            "groupId": "group-living",
            "spaceId": "space-living",
            "leaderNodeId": "node-site" if local else "node-group",
            "term": 4,
            "participantsOnline": 2,
            "participantsExpected": 2,
            "health": "healthy",
            "route": {
                "schemaVersion": 1,
                "available": True,
                "local": local,
                "address": None if local else address,
                "port": 80,
                "statePath": "/api/state",
                "lastSeenAgeMs": 200,
            },
        }],
    }


def automation_payload(
    *,
    leader: str = "node-group",
    term: int = 4,
    boot: str = "boot-a",
    sequence: int = 10,
    source_sequence: int = 20,
    available: bool = True,
):
    return {
        "automation": {
            "schemaVersion": 1,
            "complete": True,
            "health": "healthy" if available else "degraded",
            "available": available,
            "siteId": "site-main",
            "spaceId": "space-living",
            "groupId": "group-living",
            "leaderNodeId": leader,
            "bootId": boot,
            "groupTerm": term,
            "sequence": sequence,
            "sourceSequence": source_sequence,
            "ageMs": 100,
            "presence": True if available else None,
            "motion": False if available else None,
            "targetCount": 1 if available else None,
            "floorplanId": "floorplan-main",
            "areasRevision": 3,
            "areasAvailable": available,
            "areas": [{
                "areaId": "desk",
                "name": "Desk",
                "presence": True if available else None,
                "targetCount": 1 if available else None,
            }],
            "zones": [{
                "zoneId": "zone_1",
                "name": "Desk",
                "type": "normal",
                "presence": True if available else None,
                "targetCount": 1 if available else None,
            }],
        }
    }


def standalone_state_payload(*, updated_at: int = 1000):
    return {
        "version": 1,
        "connected": True,
        "updatedAt": updated_at,
        "presence": True,
        "motion": False,
        "targetCount": 1,
        "floorplanId": "floorplan-standalone",
        "areasRevision": 2,
        "areasAvailable": True,
        "areas": [{
            "areaId": "desk",
            "name": "Desk",
            "presence": True,
            "targetCount": 1,
        }],
        "zones": [{
            "id": "zone_1",
            "name": "Desk",
            "type": "detection",
            "presence": True,
            "targetCount": 1,
        }],
    }


def floorplan_payload():
    return {
        "version": 4,
        "radar": {
            "originPx": [50, 90],
            "rotationDeg": 0,
            "scale": 1,
        },
        "scale": {
            "mmPerPxX": 10,
            "mmPerPxY": 10,
        },
        "rooms": [
            {
                "id": "space-living",
                "name": "Living room",
                "pointsPx": [[0, 0], [100, 0], [100, 100], [0, 100]],
            },
            {
                "id": "space-hall",
                "name": "Hall",
                "pointsPx": [[100, 0], [150, 0], [150, 100], [100, 100]],
            },
        ],
    }


class NamoHaRouteContractTests(unittest.TestCase):
    def route(self):
        directory = routing.parse_directory(
            leader_url="http://192.0.2.10",
            site_id="site-main",
            groups_payload=directory_payload(),
        )
        return directory.routes[0]

    def test_remote_route_is_bounded_and_canonical(self):
        route = self.route()
        self.assertEqual(route.base_url, "http://192.0.2.20")
        self.assertEqual(route.state_path, "/api/state")
        self.assertEqual(route.group_term, 4)
        self.assertEqual(route.participants_online, 2)
        self.assertEqual(route.participants_expected, 2)

    def test_canonical_participant_endpoints_extend_management_fallbacks(self):
        payload = {
            "participants": [
                {
                    "canonicalMember": True,
                    "endpoint": {
                        "address": "192.0.2.31",
                        "port": 80,
                        "online": False,
                    },
                },
                {
                    "canonicalMember": True,
                    "endpoint": {"address": "192.0.2.32", "port": 8080},
                },
                {
                    "canonicalMember": False,
                    "endpoint": {"address": "192.0.2.99", "port": 80},
                },
            ]
        }
        participant_urls = routing.participant_urls_from_payload(payload)
        self.assertEqual(
            participant_urls,
            ("http://192.0.2.31", "http://192.0.2.32:8080"),
        )
        self.assertEqual(
            routing.management_candidate_urls(
                "http://192.0.2.10",
                "http://192.0.2.20",
                ("http://192.0.2.20", None),
                participant_urls,
            ),
            (
                "http://192.0.2.10",
                "http://192.0.2.20",
                "http://192.0.2.31",
                "http://192.0.2.32:8080",
            ),
        )

    def test_invalid_participant_directory_shape_fails_closed(self):
        with self.assertRaisesRegex(
            routing.RouteContractError, "participant directory is unavailable"
        ):
            routing.participant_urls_from_payload({"participants": {}})

    def test_invalid_participant_summary_is_rejected(self):
        payload = directory_payload()
        payload["siteGroups"][0]["participantsOnline"] = 3
        with self.assertRaises(routing.RouteContractError):
            routing.parse_directory(
                leader_url="192.0.2.10",
                site_id="site-main",
                groups_payload=payload,
            )

    def test_local_route_uses_directory_source(self):
        directory = routing.parse_directory(
            leader_url="192.0.2.10",
            site_id="site-main",
            groups_payload=directory_payload(local=True),
        )
        self.assertEqual(directory.routes[0].base_url, "http://192.0.2.10")

    def test_non_authoritative_directory_is_rejected(self):
        payload = directory_payload()
        payload["siteGroupSummary"]["localAuthority"] = False
        with self.assertRaises(routing.RouteContractError):
            routing.parse_directory(
                leader_url="192.0.2.10",
                site_id="site-main",
                groups_payload=payload,
            )

    def test_configured_group_is_not_removed_when_its_report_is_missing(self):
        payload = directory_payload()
        payload["siteGroupSummary"]["reportingGroups"] = 0
        entry = payload["siteGroups"][0]
        entry["participantsOnline"] = 0
        entry["health"] = "unavailable"
        entry["route"]["available"] = False
        entry["route"]["address"] = None
        entry["route"]["lastSeenAgeMs"] = None

        directory = routing.parse_directory(
            leader_url="192.0.2.10",
            site_id="site-main",
            groups_payload=payload,
        )

        self.assertEqual(len(directory.routes), 1)
        self.assertEqual(directory.routes[0].space_id, "space-living")
        self.assertFalse(directory.routes[0].available)
        self.assertIsNone(directory.routes[0].base_url)

    def test_incomplete_configured_directory_is_rejected(self):
        payload = directory_payload()
        payload["siteGroupSummary"]["reportingGroups"] = 0
        payload["siteGroups"] = []
        with self.assertRaisesRegex(
            routing.RouteContractError,
            "configured Site Groups are incomplete",
        ):
            routing.parse_directory(
                leader_url="192.0.2.10",
                site_id="site-main",
                groups_payload=payload,
            )

    def test_more_than_eight_routes_are_rejected(self):
        payload = directory_payload()
        payload["siteGroups"] *= 9
        with self.assertRaises(routing.RouteContractError):
            routing.parse_directory(
                leader_url="192.0.2.10",
                site_id="site-main",
                groups_payload=payload,
            )

    def test_duplicate_space_is_rejected(self):
        payload = directory_payload()
        duplicate = dict(payload["siteGroups"][0])
        duplicate["groupId"] = "group-other"
        payload["siteGroups"].append(duplicate)
        payload["siteGroupSummary"]["configuredGroups"] = 2
        payload["siteGroupSummary"]["reportingGroups"] = 2
        with self.assertRaises(routing.RouteContractError):
            routing.parse_directory(
                leader_url="192.0.2.10",
                site_id="site-main",
                groups_payload=payload,
            )

    def test_one_site_can_expose_multiple_space_routes(self):
        payload = directory_payload()
        second = {
            "groupId": "group-hall",
            "spaceId": "space-hall",
            "leaderNodeId": "node-hall",
            "term": 2,
            "participantsOnline": 1,
            "participantsExpected": 1,
            "health": "healthy",
            "route": {
                "schemaVersion": 1,
                "available": True,
                "local": False,
                "address": "192.0.2.30",
                "port": 80,
                "statePath": "/api/state",
                "lastSeenAgeMs": 100,
            },
        }
        payload["siteGroups"].append(second)
        payload["siteGroupSummary"]["configuredGroups"] = 2
        payload["siteGroupSummary"]["reportingGroups"] = 2
        directory = routing.parse_directory(
            leader_url="192.0.2.10",
            site_id="site-main",
            groups_payload=payload,
        )
        self.assertEqual(
            [route.space_id for route in directory.routes],
            ["space-living", "space-hall"],
        )

    def test_floorplan_names_and_radar_space_are_resolved(self):
        spaces = routing.parse_floorplan_spaces(floorplan_payload())
        self.assertEqual(spaces.names["space-living"], "Living room")
        self.assertEqual(spaces.names["space-hall"], "Hall")
        self.assertEqual(spaces.radar_space_id, "space-living")

    def test_standalone_route_uses_floorplan_space_identity(self):
        spaces = routing.parse_floorplan_spaces(floorplan_payload())
        route = routing.standalone_route(
            site_id="site-standalone",
            node_id="node-standalone",
            base_url="192.0.2.40",
            device_name="NAMO Office",
            floorplan=spaces,
        )
        self.assertTrue(route.standalone)
        self.assertEqual(route.space_id, "space-living")
        self.assertEqual(route.display_name, "Living room")

    def test_empty_standalone_directory_creates_one_local_space(self):
        directory = routing.DirectorySnapshot(
            "site-standalone", "node-standalone", ()
        )
        routes = routing.resolve_site_routes(
            directory,
            mode="standalone",
            node_id="node-standalone",
            base_url="192.0.2.40",
            device_name="Office",
            floorplan=routing.parse_floorplan_spaces(floorplan_payload()),
        )
        self.assertEqual(len(routes), 1)
        self.assertTrue(routes[0].standalone)
        self.assertEqual(routes[0].space_id, "space-living")

    def test_empty_enrolled_directory_does_not_invent_a_group(self):
        directory = routing.DirectorySnapshot("site-main", "node-site", ())
        routes = routing.resolve_site_routes(
            directory,
            mode="enrolled",
            node_id="node-site",
            base_url="192.0.2.10",
            device_name="Office",
            floorplan=routing.FloorplanSpaces({}, None),
        )
        self.assertEqual(routes, ())

    def test_standalone_without_floorplan_uses_stable_node_identity(self):
        route = routing.standalone_route(
            site_id="site-standalone",
            node_id="node-standalone",
            base_url="192.0.2.40",
            device_name="NAMO Office",
            floorplan=routing.FloorplanSpaces({}, None),
        )
        self.assertEqual(route.space_id, "standalone:node-standalone")
        self.assertEqual(route.display_name, "NAMO Office")

    def test_standalone_state_creates_available_space_snapshot(self):
        route = routing.standalone_route(
            site_id="site-standalone",
            node_id="node-standalone",
            base_url="192.0.2.40",
            device_name="Office",
            floorplan=routing.FloorplanSpaces({}, None),
        )
        snapshot = routing.parse_standalone_state(
            route, standalone_state_payload()
        )
        self.assertTrue(snapshot.available)
        self.assertTrue(snapshot.presence)
        self.assertEqual(snapshot.target_count, 1)
        self.assertEqual(snapshot.areas[0].area_id, "desk")
        self.assertEqual(snapshot.floorplan_id, "floorplan-standalone")
        self.assertEqual(snapshot.display_name, "Office")
        self.assertTrue(snapshot.standalone)
        self.assertEqual(snapshot.participants_online, 1)
        self.assertEqual(snapshot.participants_expected, 1)

    def test_standalone_without_floorplan_creates_available_snapshot(self):
        route = routing.standalone_route(
            site_id="site-standalone",
            node_id="node-standalone",
            base_url="192.0.2.40",
            device_name="Office",
            floorplan=routing.FloorplanSpaces({}, None),
        )
        previous = routing.parse_standalone_state(
            route, standalone_state_payload()
        )
        payload = standalone_state_payload(updated_at=2000)
        payload.update({
            "floorplanId": None,
            "areasRevision": 0,
            "areasAvailable": False,
            "areas": [],
        })

        snapshot = routing.parse_standalone_state(route, payload, previous)

        self.assertTrue(snapshot.available)
        self.assertTrue(snapshot.presence)
        self.assertEqual(snapshot.target_count, 1)
        self.assertIsNone(snapshot.floorplan_id)
        self.assertEqual(snapshot.areas_revision, 0)
        self.assertFalse(snapshot.areas_available)
        self.assertEqual(snapshot.areas, ())

    def test_standalone_rejects_incomplete_no_floorplan_contract(self):
        route = routing.standalone_route(
            site_id="site-standalone",
            node_id="node-standalone",
            base_url="192.0.2.40",
            device_name="Office",
            floorplan=routing.FloorplanSpaces({}, None),
        )
        invalid_values = (
            ("areasRevision", 1),
            ("areasAvailable", True),
            ("areas", [{
                "areaId": "desk",
                "name": "Desk",
                "presence": None,
                "targetCount": None,
            }]),
        )

        for field, value in invalid_values:
            with self.subTest(field=field):
                payload = standalone_state_payload()
                payload.update({
                    "floorplanId": None,
                    "areasRevision": 0,
                    "areasAvailable": False,
                    "areas": [],
                    field: value,
                })
                with self.assertRaises(routing.RouteContractError):
                    routing.parse_standalone_state(route, payload)

    def test_standalone_disconnect_is_unavailable_not_empty(self):
        route = routing.standalone_route(
            site_id="site-standalone",
            node_id="node-standalone",
            base_url="192.0.2.40",
            device_name="Office",
            floorplan=routing.FloorplanSpaces({}, None),
        )
        payload = standalone_state_payload()
        payload["connected"] = False
        snapshot = routing.parse_standalone_state(route, payload)
        self.assertFalse(snapshot.available)
        self.assertIsNone(snapshot.presence)
        self.assertIsNone(snapshot.target_count)

    def test_authoritative_state_is_admitted(self):
        snapshot = routing.parse_automation_state(
            self.route(), automation_payload()
        )
        self.assertTrue(snapshot.available)
        self.assertTrue(snapshot.presence)
        self.assertFalse(snapshot.motion)
        self.assertEqual(snapshot.target_count, 1)
        self.assertEqual(snapshot.areas[0].area_id, "desk")
        self.assertEqual(snapshot.floorplan_id, "floorplan-main")
        self.assertFalse(snapshot.standalone)
        self.assertEqual(snapshot.participants_online, 2)
        self.assertEqual(snapshot.participants_expected, 2)
        self.assertEqual(snapshot.source_age_ms, 100)

    def test_partial_group_keeps_state_available_but_reports_degraded(self):
        payload = directory_payload()
        payload["siteGroups"][0]["participantsOnline"] = 1
        payload["siteGroups"][0]["health"] = "degraded"
        route = routing.parse_directory(
            leader_url="192.0.2.10",
            site_id="site-main",
            groups_payload=payload,
        ).routes[0]
        snapshot = routing.parse_automation_state(route, automation_payload())
        self.assertTrue(snapshot.available)
        self.assertEqual(snapshot.health, "degraded")

    def test_site_summary_counts_spaces_devices_and_problems(self):
        healthy = routing.parse_automation_state(
            self.route(), automation_payload()
        )
        degraded = routing.replace(
            healthy,
            space_id="space-hall",
            display_name="Hall",
            health="degraded",
            participants_online=1,
            participants_expected=2,
        )
        site = routing.summarize_site(
            "site-main",
            {"space-living": healthy, "space-hall": degraded},
        )
        self.assertEqual(site.health, "degraded")
        self.assertEqual(site.spaces_available, 2)
        self.assertEqual(site.devices_online, 3)
        self.assertEqual(site.devices_expected, 4)
        self.assertEqual(site.problem_spaces, ("Hall",))

    def test_wrong_leader_is_rejected(self):
        with self.assertRaises(routing.RouteContractError):
            routing.parse_automation_state(
                self.route(), automation_payload(leader="node-other")
            )

    def test_unavailable_never_becomes_false(self):
        snapshot = routing.parse_automation_state(
            self.route(), automation_payload(available=False)
        )
        self.assertFalse(snapshot.available)
        self.assertIsNone(snapshot.presence)
        self.assertIsNone(snapshot.motion)
        self.assertIsNone(snapshot.target_count)
        self.assertEqual(snapshot.unavailable_reason, "automation_degraded")

    def test_timeout_preserves_area_identity_as_unavailable(self):
        route = self.route()
        previous = routing.parse_automation_state(route, automation_payload())
        snapshot = routing.unavailable_snapshot(route, previous)
        self.assertEqual(snapshot.areas[0].area_id, "desk")
        self.assertIsNone(snapshot.areas[0].presence)
        self.assertEqual(snapshot.unavailable_reason, "unavailable")

    def test_transient_transport_failure_preserves_last_good_state(self):
        route = self.route()
        previous = routing.parse_automation_state(route, automation_payload())
        guard = availability.SnapshotAvailabilityGuard(10)

        snapshot, preserved = guard.reject(
            route,
            previous,
            reason="request_failed",
            now=100,
        )

        self.assertTrue(preserved)
        self.assertTrue(snapshot.available)
        self.assertEqual(snapshot.health, "degraded")
        self.assertTrue(snapshot.presence)
        self.assertEqual(snapshot.target_count, 1)
        self.assertEqual(snapshot.unavailable_reason, "request_failed")

    def test_transport_failure_becomes_unavailable_after_grace(self):
        route = self.route()
        previous = routing.parse_automation_state(route, automation_payload())
        guard = availability.SnapshotAvailabilityGuard(10)
        preserved_snapshot, _ = guard.reject(
            route,
            previous,
            reason="request_failed",
            now=100,
        )

        snapshot, still_preserved = guard.reject(
            route,
            preserved_snapshot,
            reason="request_failed",
            now=110,
        )

        self.assertFalse(still_preserved)
        self.assertFalse(snapshot.available)
        self.assertIsNone(snapshot.presence)
        self.assertEqual(snapshot.areas[0].area_id, "desk")
        self.assertIsNone(snapshot.areas[0].presence)

    def test_valid_snapshot_immediately_resets_failure_window(self):
        route = self.route()
        first = routing.parse_automation_state(route, automation_payload())
        guard = availability.SnapshotAvailabilityGuard(10)
        guard.reject(route, first, reason="request_failed", now=100)
        recovered = routing.parse_automation_state(
            route, automation_payload(sequence=11, source_sequence=21)
        )
        guard.accept(recovered)

        snapshot, preserved = guard.reject(
            route,
            recovered,
            reason="request_failed",
            now=120,
        )

        self.assertTrue(preserved)
        self.assertTrue(snapshot.available)
        self.assertEqual(snapshot.sequence, 11)

    def test_failure_grace_bridges_forward_authority_handoff(self):
        old_route = self.route()
        previous = routing.parse_automation_state(
            old_route, automation_payload()
        )
        new_route = routing.GroupRoute(
            site_id=old_route.site_id,
            space_id=old_route.space_id,
            group_id=old_route.group_id,
            leader_node_id="node-new",
            group_term=old_route.group_term + 1,
            base_url="http://192.0.2.30",
            state_path="/api/state",
            available=True,
        )
        guard = availability.SnapshotAvailabilityGuard(10)

        snapshot, preserved = guard.reject(
            new_route,
            previous,
            reason="request_failed",
            now=100,
        )

        self.assertTrue(preserved)
        self.assertTrue(snapshot.available)
        self.assertTrue(snapshot.presence)
        self.assertTrue(snapshot.continuity_held)

    def test_failure_grace_rejects_backward_authority(self):
        old_route = self.route()
        previous = routing.parse_automation_state(
            old_route, automation_payload()
        )
        stale_route = dataclasses.replace(
            old_route,
            leader_node_id="node-stale",
            group_term=old_route.group_term - 1,
        )
        guard = availability.SnapshotAvailabilityGuard(10)

        snapshot, preserved = guard.reject(
            stale_route,
            previous,
            reason="request_failed",
            now=100,
        )

        self.assertFalse(preserved)
        self.assertFalse(snapshot.available)
        self.assertIsNone(snapshot.presence)

    def test_explicit_recovering_snapshot_is_read_only_and_available(self):
        route = dataclasses.replace(
            self.route(), leader_node_id="node-new", group_term=5
        )
        payload = automation_payload(
            leader="node-new", term=5, boot="boot-old", sequence=10
        )
        payload["automation"].update({
            "health": "recovering",
            "available": True,
            "ageMs": 12000,
            "continuityHeld": True,
            "holdAgeMs": 11000,
            "holdRemainingMs": 19000,
            "continuitySourceLeaderNodeId": "node-old",
            "continuitySourceGroupTerm": 4,
        })

        snapshot = routing.parse_automation_state(route, payload)

        self.assertTrue(snapshot.available)
        self.assertEqual(snapshot.health, "degraded")
        self.assertTrue(snapshot.presence)
        self.assertFalse(snapshot.motion)
        self.assertTrue(snapshot.continuity_held)
        self.assertEqual(snapshot.hold_remaining_ms, 19000)
        self.assertEqual(snapshot.unavailable_reason, "automation_recovering")

    def test_explicit_recovering_accepts_full_hold_with_fresh_source(self):
        route = dataclasses.replace(
            self.route(), leader_node_id="node-new", group_term=5
        )
        payload = automation_payload(
            leader="node-new", term=5, boot="boot-old", sequence=10
        )
        payload["automation"].update({
            "health": "recovering",
            "available": True,
            "ageMs": 34999,
            "continuityHeld": True,
            "holdAgeMs": 29999,
            "holdRemainingMs": 1,
            "continuitySourceLeaderNodeId": "node-old",
            "continuitySourceGroupTerm": 4,
        })

        snapshot = routing.parse_automation_state(route, payload)

        self.assertTrue(snapshot.available)
        self.assertTrue(snapshot.continuity_held)

    def test_explicit_recovering_rejects_stale_source_before_hold(self):
        route = dataclasses.replace(
            self.route(), leader_node_id="node-new", group_term=5
        )
        payload = automation_payload(
            leader="node-new", term=5, boot="boot-old", sequence=10
        )
        payload["automation"].update({
            "health": "recovering",
            "available": True,
            "ageMs": 35000,
            "continuityHeld": True,
            "holdAgeMs": 29000,
            "holdRemainingMs": 1000,
            "continuitySourceLeaderNodeId": "node-old",
            "continuitySourceGroupTerm": 4,
        })

        with self.assertRaises(routing.RouteContractError):
            routing.parse_automation_state(route, payload)

    def test_explicit_recovering_rejects_newer_source_authority(self):
        route = dataclasses.replace(
            self.route(), leader_node_id="node-new", group_term=5
        )
        payload = automation_payload(
            leader="node-new", term=5, boot="boot-old", sequence=10
        )
        payload["automation"].update({
            "health": "recovering",
            "available": True,
            "ageMs": 12000,
            "continuityHeld": True,
            "holdAgeMs": 11000,
            "holdRemainingMs": 19000,
            "continuitySourceLeaderNodeId": "node-impossible",
            "continuitySourceGroupTerm": 6,
        })

        with self.assertRaises(routing.RouteContractError):
            routing.parse_automation_state(route, payload)

    def test_transport_failure_does_not_extend_explicit_recovery_hold(self):
        route = dataclasses.replace(
            self.route(), leader_node_id="node-new", group_term=5
        )
        previous = dataclasses.replace(
            routing.parse_automation_state(
                route,
                automation_payload(
                    leader="node-new", term=5, boot="boot-old", sequence=10
                ),
            ),
            health="degraded",
            unavailable_reason="automation_recovering",
            continuity_held=True,
            hold_remaining_ms=5000,
            source_age_ms=25000,
        )
        guard = availability.SnapshotAvailabilityGuard(30)

        held, preserved = guard.reject(
            route, previous, reason="request_failed", now=100
        )
        expired, expired_preserved = guard.reject(
            route, held, reason="request_failed", now=105.1
        )

        self.assertTrue(preserved)
        self.assertLessEqual(held.hold_remaining_ms or 0, 5000)
        self.assertFalse(expired_preserved)
        self.assertFalse(expired.available)

    def test_route_unavailable_uses_the_same_bounded_grace(self):
        route = self.route()
        previous = routing.parse_automation_state(route, automation_payload())
        unavailable_route = routing.GroupRoute(
            site_id=route.site_id,
            space_id=route.space_id,
            group_id=route.group_id,
            leader_node_id=route.leader_node_id,
            group_term=route.group_term,
            base_url=None,
            state_path=route.state_path,
            available=False,
        )
        guard = availability.SnapshotAvailabilityGuard(10)

        snapshot, preserved = guard.reject(
            unavailable_route,
            previous,
            reason="route_unavailable",
            now=100,
        )

        self.assertTrue(preserved)
        self.assertTrue(snapshot.available)
        self.assertEqual(snapshot.unavailable_reason, "route_unavailable")

    def test_reordered_sequence_is_rejected(self):
        route = self.route()
        fence = routing.SnapshotFence()
        fence.accept(routing.parse_automation_state(route, automation_payload()))
        older = routing.parse_automation_state(
            route, automation_payload(sequence=9, source_sequence=19)
        )
        with self.assertRaises(routing.RouteContractError):
            fence.accept(older)

    def test_duplicate_sequence_reuses_previous_snapshot(self):
        route = self.route()
        fence = routing.SnapshotFence()
        first = fence.accept(
            routing.parse_automation_state(route, automation_payload())
        )
        duplicate = fence.accept(
            routing.parse_automation_state(route, automation_payload())
        )
        self.assertIs(first, duplicate)

    def test_area_revision_update_is_admitted_without_new_fusion_sequence(self):
        route = self.route()
        fence = routing.SnapshotFence()
        first = fence.accept(
            routing.parse_automation_state(route, automation_payload())
        )
        payload = automation_payload()
        payload["automation"]["areasRevision"] = 4
        payload["automation"]["areas"][0]["name"] = "Work desk"
        changed = fence.accept(routing.parse_automation_state(route, payload))
        self.assertIsNot(first, changed)
        self.assertEqual(changed.areas_revision, 4)
        self.assertEqual(changed.areas[0].name, "Work desk")

    def test_new_leader_at_same_term_is_admitted_from_new_route(self):
        old_route = self.route()
        fence = routing.SnapshotFence()
        fence.accept(
            routing.parse_automation_state(old_route, automation_payload())
        )
        new_route = routing.GroupRoute(
            site_id=old_route.site_id,
            space_id=old_route.space_id,
            group_id=old_route.group_id,
            leader_node_id="node-new",
            group_term=old_route.group_term,
            base_url="http://192.0.2.30",
            state_path="/api/state",
            available=True,
        )
        accepted = fence.accept(
            routing.parse_automation_state(
                new_route,
                automation_payload(leader="node-new", boot="boot-new", sequence=1),
            )
        )
        self.assertEqual(accepted.leader_node_id, "node-new")

    def test_restarted_leader_may_reset_runtime_term(self):
        old_route = self.route()
        fence = routing.SnapshotFence()
        fence.accept(
            routing.parse_automation_state(old_route, automation_payload())
        )
        restarted_route = dataclasses.replace(old_route, group_term=1)
        accepted = fence.accept(
            routing.parse_automation_state(
                restarted_route,
                automation_payload(term=1, boot="boot-restarted", sequence=1),
            )
        )
        self.assertEqual(accepted.group_term, 1)
        self.assertEqual(accepted.boot_id, "boot-restarted")

    def test_same_boot_cannot_move_runtime_term_backwards(self):
        old_route = self.route()
        fence = routing.SnapshotFence()
        fence.accept(
            routing.parse_automation_state(old_route, automation_payload())
        )
        regressed_route = dataclasses.replace(old_route, group_term=1)
        with self.assertRaises(routing.RouteContractError):
            fence.accept(
                routing.parse_automation_state(
                    regressed_route,
                    automation_payload(term=1, boot="boot-a", sequence=11),
                )
            )


class NamoHaPackageTests(unittest.TestCase):
    def test_manifest_and_translations_are_valid_json(self):
        for path in (
            PACKAGE_ROOT / "manifest.json",
            PACKAGE_ROOT / "strings.json",
            PACKAGE_ROOT / "translations" / "en.json",
            PACKAGE_ROOT / "translations" / "ko.json",
            ROOT / "packaging" / "home-assistant" / "hacs.json",
        ):
            with self.subTest(path=path):
                json.loads(path.read_text(encoding="utf-8"))

    def test_manifest_declares_local_polling_and_discovery(self):
        manifest = json.loads(
            (PACKAGE_ROOT / "manifest.json").read_text(encoding="utf-8")
        )
        self.assertEqual(manifest["domain"], "namo")
        self.assertEqual(manifest["iot_class"], "local_polling")
        self.assertTrue(manifest["config_flow"])
        self.assertEqual(manifest["zeroconf"][0]["type"], "_namo._tcp.local.")

    def test_space_unique_ids_are_site_scoped_and_stable(self):
        first = entity_model.space_entity_unique_id(
            "site-main", "space-living", "occupancy"
        )
        same_after_handover = entity_model.space_entity_unique_id(
            "site-main", "space-living", "occupancy"
        )
        other_site = entity_model.space_entity_unique_id(
            "site-other", "space-living", "occupancy"
        )
        self.assertEqual(first, same_after_handover)
        self.assertNotEqual(first, other_site)

    def test_area_unique_id_survives_owner_handover(self):
        first = entity_model.area_entity_unique_id(
            "site-main", "floorplan-main", "desk", "occupancy"
        )
        after_handover = entity_model.area_entity_unique_id(
            "site-main", "floorplan-main", "desk", "occupancy"
        )
        self.assertEqual(first, after_handover)
        self.assertNotIn("node-", first)

    def test_device_name_uses_floorplan_name_without_changing_identity(self):
        route = routing.GroupRoute(
            site_id="site-main",
            space_id="space-living",
            group_id="group-living",
            leader_node_id="node-group",
            group_term=4,
            base_url="http://192.0.2.20",
            state_path="/api/state",
            available=True,
            display_name="Living room",
        )
        snapshot = routing.parse_automation_state(route, automation_payload())
        self.assertEqual(
            entity_model.space_device_name(snapshot, route.space_id),
            "NAMO Living room",
        )
        self.assertEqual(
            entity_model.space_device_identifier(route.site_id, route.space_id),
            "site-main:space-living",
        )

    def test_legacy_group_zone_entities_are_selected_for_removal(self):
        self.assertTrue(
            entity_model.is_legacy_zone_unique_id(
                "site-main:space-living:zone:zone_1:occupancy"
            )
        )
        self.assertFalse(
            entity_model.is_legacy_zone_unique_id(
                "site-main:space-living:occupancy"
            )
        )


if __name__ == "__main__":
    unittest.main()

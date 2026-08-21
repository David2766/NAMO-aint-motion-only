from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "site_leader_handover_smoke.py"
ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("site_leader_handover_smoke", SCRIPT)
smoke = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = smoke
SPEC.loader.exec_module(smoke)


def snapshot(node_id: str, peer_id: str, leader_id: str,
             *, local_leader: bool, health: str = "healthy",
             group_leader_id: str | None = None,
             local_group_leader: bool | None = None):
    group_leader_id = group_leader_id or leader_id
    local_group_leader = (
        local_leader if local_group_leader is None else local_group_leader
    )
    return smoke.DeviceSnapshot(
        base_url=f"http://{node_id}",
        node_id=node_id,
        site_id="site-main",
        participants={
            "participants": [{
                "participantId": peer_id,
                "membership": "active",
                "readiness": {"ready": True},
                "credential": {"provisioned": True, "enabled": True},
                "endpoint": {"online": health == "healthy"},
            }]
        },
        groups={
            "site": {
                "term": 3,
                "leaderNodeId": leader_id,
                "localLeader": local_leader,
                "configurationReady": True,
                "health": health,
            },
            "groups": [{
                "groupId": "room-main",
                "configured": True,
                "term": 3,
                "leaderNodeId": group_leader_id,
                "localLeader": local_group_leader,
                "health": health,
            }],
            "siteGroups": ([{
                "groupId": "room-main",
                "spaceId": "space-a",
                "leaderNodeId": group_leader_id,
                "route": {
                    "schemaVersion": 1,
                    "available": health == "healthy",
                    "local": group_leader_id == node_id,
                    "address": (
                        None if group_leader_id == node_id else "192.0.2.2"
                    ),
                    "port": 80,
                    "statePath": "/api/state",
                    "lastSeenAgeMs": 0,
                },
            }] if local_leader else []),
        },
        configuration={
            "siteId": "site-main",
            "generation": 4,
            "rosterRevision": 3,
            "configurationOwnerNodeId": "node-a",
            "localOwner": node_id == "node-a",
            "members": [
                {
                    "participantId": "node-a",
                    "participantType": "namo_node",
                    "membership": "active",
                    "leaderEligible": True,
                },
                {
                    "participantId": "node-b",
                    "participantType": "namo_node",
                    "membership": "active",
                    "leaderEligible": True,
                },
            ],
            "floorplan": {
                "committed": True,
                "floorplanId": "floorplan-main",
                "geometryVersion": 1,
                "configHash": 10,
                "imageHash": 20,
            },
        },
    )


class FakeClient:
    def __init__(self, snapshots, configurations=None):
        self.snapshots = iter(snapshots)
        self.configurations = iter(configurations or [])

    def snapshot(self):
        return next(self.snapshots)

    def get(self, path):
        if path != "/api/platform/site-configuration":
            raise AssertionError(path)
        return next(self.configurations)


class SiteLeaderHandoverSmokeTests(unittest.TestCase):
    def test_automatic_owner_handover_reconciles_canonical_assignments(self):
        source = (
            ROOT / "components" / "radar_api_server" / "radar_api_server.cpp"
        ).read_text(encoding="utf-8")
        handover_start = source.index(
            "void RadarApiServer::process_automatic_configuration_handover_"
        )
        handover_end = source.index(
            "void RadarApiServer::process_spatial_readiness_invalidation_",
            handover_start,
        )
        handover = source[handover_start:handover_end]
        self.assertIn(
            'reconcile_owned_spatial_assignments_("automatic_handover")',
            handover,
        )
        self.assertIn(
            'reconcile_owned_spatial_assignments_("boot")',
            source,
        )

    def test_owner_handover_refreshes_credentials_without_resetting_runtime(self):
        source = (
            ROOT / "components" / "radar_api_server" / "radar_api_server.cpp"
        ).read_text(encoding="utf-8")
        provisioning_start = source.index(
            "void RadarApiServer::process_automatic_credential_provisioning_"
        )
        provisioning_end = source.index(
            "bool RadarApiServer::site_credential_identity_ready_",
            provisioning_start,
        )
        provisioning = source[provisioning_start:provisioning_end]
        authority_start = provisioning.index(
            "CredentialProvisioningContextChange::AUTHORITY_REFRESH"
        )
        authority_end = provisioning.index("} else {", authority_start)
        authority_refresh = provisioning[authority_start:authority_end]

        self.assertNotIn("reset_site_scoped_runtime_", authority_refresh)
        self.assertIn("SiteReplicationTarget::CREDENTIAL_BUNDLE", authority_refresh)
        self.assertIn("site_replication_->cancel()", authority_refresh)
        self.assertIn("Site/Group runtime preserved", authority_refresh)

        identity_start = source.index(
            "bool RadarApiServer::site_credential_identity_ready_"
        )
        identity_end = source.index(
            "RadarApiServer::reconcile_standalone_site_", identity_start
        )
        identity_ready = source[identity_start:identity_end]
        self.assertIn("site_credentials_complete_for_node", identity_ready)
        self.assertIn("committed_identity", identity_ready)

    def test_active_pair_requires_reciprocal_ready_credentials_and_leader_agreement(self):
        first = snapshot(
            "node-a", "node-b", "node-a", local_leader=True,
            group_leader_id="node-b", local_group_leader=False
        )
        second = snapshot(
            "node-b", "node-a", "node-a", local_leader=False,
            group_leader_id="node-b", local_group_leader=True
        )
        self.assertEqual(smoke.validate_active_pair(first, second), "node-a")

        second.participants["participants"][0]["credential"]["enabled"] = False
        with self.assertRaisesRegex(RuntimeError, "credential is not enabled"):
            smoke.validate_active_pair(first, second)

        second.participants["participants"][0]["credential"]["enabled"] = True
        second.configuration["configurationConsistent"] = False
        with self.assertRaisesRegex(RuntimeError, "fenced Site configuration"):
            smoke.validate_active_pair(first, second)

    def test_takeover_requires_local_leader_and_degraded_roster(self):
        waiting = snapshot("node-b", "node-a", "node-a", local_leader=False,
                           health="degraded", group_leader_id="node-b",
                           local_group_leader=True)
        takeover = snapshot("node-b", "node-a", "node-b", local_leader=True,
                            health="degraded")
        result = smoke.wait_for_takeover(
            FakeClient([waiting, takeover]), "node-a", 2, sleep=lambda _: None
        )
        self.assertEqual(result.node_id, "node-b")

    def test_active_pair_rejects_role_overlap_when_two_candidates_exist(self):
        first = snapshot("node-a", "node-b", "node-a", local_leader=True)
        second = snapshot("node-b", "node-a", "node-a", local_leader=False)
        with self.assertRaisesRegex(RuntimeError, "must not share"):
            smoke.validate_active_pair(first, second)

    def test_fusion_snapshot_requires_local_authority_and_authenticated_source(self):
        leader = snapshot(
            "node-b", "node-a", "node-a", local_leader=False,
            group_leader_id="node-b", local_group_leader=True
        )
        follower = snapshot(
            "node-a", "node-b", "node-a", local_leader=True,
            group_leader_id="node-b", local_group_leader=False
        )
        follower.participants["shadowTransport"] = {
            "fusionSnapshot": {
                "admitted": 4,
                "rejected": 0,
                "source": "node-b",
            }
        }

        def active_state(source):
            return {
                "fusion": {
                    "buildEnabled": True,
                    "authoritative": True,
                    "state": "active",
                },
                "automation": {
                    "schemaVersion": 1,
                    "complete": True,
                    "health": "healthy",
                    "available": True,
                    "siteId": "site-main",
                    "spaceId": "space-a",
                    "groupId": "room-main",
                    "leaderNodeId": "node-b",
                    "bootId": "boot-a",
                    "source": source,
                    "groupTerm": 1,
                    "sequence": 4,
                    "sourceSequence": 7,
                    "zones": [],
                },
            }
        smoke.validate_fusion_snapshot_state(
            leader, follower, active_state("local_group_leader"),
            active_state("group_leader_snapshot")
        )

        follower.groups["siteGroups"][0]["route"]["available"] = False
        with self.assertRaisesRegex(RuntimeError, "route"):
            smoke.validate_fusion_snapshot_state(
                leader, follower, active_state("local_group_leader"),
                active_state("group_leader_snapshot")
            )
        follower.groups["siteGroups"][0]["route"]["available"] = True

        follower.participants["shadowTransport"]["fusionSnapshot"]["source"] = "node-c"
        with self.assertRaisesRegex(RuntimeError, "source"):
            smoke.validate_fusion_snapshot_state(
                leader, follower, active_state("local_group_leader"),
                active_state("group_leader_snapshot")
            )

    def test_handover_requires_canonical_leader_eligibility_on_both_nodes(self):
        eligible = {
            "members": [{
                "participantId": "node-b",
                "membership": "active",
                "leaderEligible": True,
            }]
        }
        smoke.validate_handover_eligibility(eligible, eligible, "node-b")

        ineligible = {
            "members": [{
                "participantId": "node-b",
                "membership": "active",
                "leaderEligible": False,
            }]
        }
        with self.assertRaisesRegex(RuntimeError, "not Leader-eligible"):
            smoke.validate_handover_eligibility(
                eligible, ineligible, "node-b"
            )

    def test_configuration_successor_is_deterministic(self):
        configuration = {
            "configurationOwnerNodeId": "node-owner",
            "members": [
                {
                    "participantId": "node-z",
                    "participantType": "namo_node",
                    "membership": "active",
                    "leaderEligible": True,
                },
                {
                    "participantId": "node-a",
                    "participantType": "namo_node",
                    "membership": "active",
                    "leaderEligible": True,
                },
                {
                    "participantId": "node-owner",
                    "participantType": "namo_node",
                    "membership": "active",
                    "leaderEligible": True,
                },
            ],
        }
        self.assertEqual(
            smoke.expected_configuration_successor(configuration), "node-a"
        )

    def test_configuration_owner_wait_reports_and_accepts_promoted_owner(self):
        waiting = {
            "configurationOwnerNodeId": "node-a",
            "localOwner": False,
            "automaticHandover": {
                "state": "waiting",
                "condition": "eligible",
            },
        }
        promoted = {
            "configurationOwnerNodeId": "node-b",
            "localOwner": True,
            "automaticHandover": {
                "state": "complete",
                "condition": "eligible",
            },
        }
        result = smoke.wait_for_configuration_owner(
            FakeClient([], [waiting, promoted]),
            "node-b",
            2,
            sleep=lambda _: None,
        )
        self.assertEqual(result["configurationOwnerNodeId"], "node-b")


if __name__ == "__main__":
    unittest.main()

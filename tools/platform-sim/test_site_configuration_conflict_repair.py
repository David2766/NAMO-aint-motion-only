from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "site_configuration_conflict_repair.py"
SPEC = importlib.util.spec_from_file_location(
    "site_configuration_conflict_repair", SCRIPT
)
repair = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = repair
SPEC.loader.exec_module(repair)


def snapshot(node_id: str, owner_id: str, digest: str,
             *, consistent: bool = False,
             config_hash: int = 10) -> repair.DeviceSnapshot:
    site = {
        "schemaVersion": 2,
        "siteId": "site-main",
        "authorityEpoch": 1,
        "generation": 59,
        "configurationDigest": digest,
        "configurationConsistent": consistent,
        "configurationOwnerNodeId": owner_id,
        "localOwner": node_id == owner_id,
        "members": [
            {"participantId": "node-a", "membership": "active"},
            {"participantId": "node-b", "membership": "active"},
        ],
        "floorplan": {
            "floorplanId": "floorplan-main",
            "geometryVersion": 3,
            "configHash": config_hash,
            "imageHash": 20,
        },
    }
    return repair.DeviceSnapshot(
        base_url=f"http://{node_id}",
        node_id=node_id,
        site=site,
        payloads={
            "/api/platform/site-configuration": site,
            "/api/platform/groups": {
                "site": {
                    "leaderNodeId": "node-a",
                    "term": 3,
                    "health": "healthy",
                },
                "groups": [{
                    "groupId": "room-main",
                    "configured": True,
                    "leaderNodeId": "node-b",
                    "term": 4,
                    "health": "healthy",
                }],
            },
        },
    )


class SiteConfigurationConflictRepairTests(unittest.TestCase):
    def test_selected_local_owner_can_repair_an_equal_revision_split(self):
        source = snapshot("node-a", "node-a", "1111111111111111")
        peer = snapshot("node-b", "node-b", "2222222222222222")
        repair.validate_repair_pair(source, peer)

    def test_repair_rejects_different_active_rosters(self):
        source = snapshot("node-a", "node-a", "1111111111111111")
        peer = snapshot("node-b", "node-b", "2222222222222222")
        peer.site["members"].pop()
        with self.assertRaisesRegex(RuntimeError, "rosters differ"):
            repair.validate_repair_pair(source, peer)

    def test_convergence_requires_fence_digest_owner_and_floorplan(self):
        source = snapshot(
            "node-a", "node-a", "3333333333333333", consistent=True
        )
        peer = snapshot(
            "node-b", "node-a", "3333333333333333", consistent=True
        )
        self.assertTrue(repair.converged(source, peer))
        peer.site["floorplan"]["configHash"] = 99
        self.assertFalse(repair.converged(source, peer))

    def test_convergence_requires_site_and_group_leader_agreement(self):
        source = snapshot(
            "node-a", "node-a", "3333333333333333", consistent=True
        )
        peer = snapshot(
            "node-b", "node-a", "3333333333333333", consistent=True
        )
        peer.payloads["/api/platform/groups"]["groups"][0][
            "leaderNodeId"
        ] = "node-a"
        self.assertFalse(repair.converged(source, peer))


if __name__ == "__main__":
    unittest.main()

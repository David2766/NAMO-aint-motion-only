from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "office_noise_capture.py"
SPEC = importlib.util.spec_from_file_location("office_noise_capture", SCRIPT)
capture = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = capture
SPEC.loader.exec_module(capture)


class FakeClient:
    def __init__(self, payload=None, error=None):
        self.payload = payload or {}
        self.error = error

    def get_json(self, path):
        if self.error is not None:
            raise self.error
        return self.payload, 12.5


class OfficeNoiseCaptureTests(unittest.TestCase):
    def test_device_parser_derives_ip_alias_and_accepts_named_alias(self):
        derived = capture.parse_device_spec("http://192.168.29.218")
        named = capture.parse_device_spec(
            "leader=http://192.168.29.104/"
        )
        self.assertEqual(derived.alias, "218")
        self.assertEqual(derived.base_url, "http://192.168.29.218")
        self.assertEqual(named.alias, "leader")
        self.assertEqual(named.base_url, "http://192.168.29.104")

    def test_device_parser_rejects_duplicate_and_path_urls(self):
        with self.assertRaisesRegex(ValueError, "aliases"):
            capture.parse_devices([
                "same=http://192.168.29.78",
                "same=http://192.168.29.104",
            ])
        with self.assertRaisesRegex(ValueError, "path"):
            capture.parse_device_spec("http://192.168.29.78/api/state")

    def test_compact_state_keeps_local_and_authoritative_targets(self):
        compact = capture.compact_state({
            "connected": True,
            "presence": True,
            "targetCount": 1,
            "targets": [{
                "id": "target_1",
                "x": 120,
                "y": 340,
                "active": True,
                "fused": True,
                "sourceCount": 3,
            }],
            "fusion": {
                "authoritative": True,
                "state": "active",
                "source": "group_leader_snapshot",
            },
            "local": {
                "presence": True,
                "targetCount": 1,
                "targets": [{"x": -50, "y": 800, "active": True}],
            },
            "automation": {
                "available": True,
                "health": "recovering",
                "continuityHeld": True,
                "holdAgeMs": 1200,
                "holdRemainingMs": 28800,
                "continuitySourceLeaderNodeId": "node-old",
                "continuitySourceGroupTerm": 7,
                "leaderNodeId": "node-new",
                "groupTerm": 8,
            },
            "debug": {
                "presenceReason": "tracker",
                "staticRadar": {
                    "available": True,
                    "presence": False,
                    "assist": {"active": False},
                },
            },
        })
        self.assertEqual(compact["targets"][0]["sourceCount"], 3)
        self.assertTrue(compact["targets"][0]["fused"])
        self.assertEqual(compact["local"]["targets"][0]["x"], -50)
        self.assertEqual(compact["fusion"]["source"], "group_leader_snapshot")
        self.assertTrue(compact["automation"]["continuityHeld"])
        self.assertEqual(compact["automation"]["holdAgeMs"], 1200)
        self.assertEqual(compact["automation"]["holdRemainingMs"], 28800)
        self.assertEqual(
            compact["automation"]["continuitySourceLeaderNodeId"],
            "node-old",
        )
        self.assertEqual(
            compact["automation"]["continuitySourceGroupTerm"], 7
        )

    def test_compact_groups_keeps_leader_and_pairing_diagnostics(self):
        compact = capture.compact_groups({
            "site": {
                "leaderNodeId": "node-b",
                "localLeader": False,
                "term": 4,
                "participantsOnline": 4,
                "participantsExpected": 4,
                "quorumRequired": 3,
                "quorumAvailable": True,
                "health": "healthy",
            },
            "groups": [{
                "configured": True,
                "groupId": "room",
                "leaderNodeId": "node-a",
                "localLeader": True,
                "term": 7,
                "participantsOnline": 4,
                "participantsExpected": 4,
                "quorumRequired": 3,
                "quorumAvailable": True,
                "canonicalAssignment": True,
                "credentialsComplete": True,
                "health": "healthy",
            }],
            "shadowFusion": {
                "state": "active",
                "trackCount": 2,
                "diagnostics": {
                    "validInputs": 4,
                    "candidatePairs": 3,
                    "pairedMeasurements": 2,
                    "ambiguousPairs": 1,
                    "droppedMeasurements": 0,
                },
            },
            "productionFusion": {
                "authoritative": True,
                "state": "active",
                "targetCount": 2,
            },
        })
        self.assertTrue(compact["groups"][0]["localLeader"])
        self.assertEqual(compact["site"]["leaderNodeId"], "node-b")
        self.assertEqual(compact["site"]["term"], 4)
        self.assertTrue(compact["site"]["quorumAvailable"])
        self.assertTrue(compact["groups"][0]["canonicalAssignment"])
        self.assertEqual(compact["shadowFusion"]["validInputs"], 4)
        self.assertEqual(compact["shadowFusion"]["pairedMeasurements"], 2)

    def test_compact_configuration_keeps_owner_fence(self):
        compact = capture.compact_configuration({
            "siteId": "site-main",
            "configurationOwnerNodeId": "node-c",
            "localOwner": True,
            "authorityEpoch": 3,
            "generation": 17,
            "rosterRevision": 9,
            "configurationConsistent": True,
            "sharedWritesAllowed": True,
            "memberCount": 5,
            "activeMemberCount": 5,
        })
        self.assertEqual(compact["configurationOwnerNodeId"], "node-c")
        self.assertEqual(compact["authorityEpoch"], 3)
        self.assertEqual(compact["generation"], 17)

    def test_collection_preserves_success_and_failure_per_device(self):
        devices = capture.parse_devices([
            "a=http://192.168.29.78",
            "b=http://192.168.29.104",
        ])
        clients = {
            "a": FakeClient({"connected": True}),
            "b": FakeClient(error=TimeoutError("simulated timeout")),
        }
        with ThreadPoolExecutor(max_workers=2) as executor:
            result, errors = capture.collect_endpoint(
                executor,
                devices,
                clients,
                "/api/state",
                capture.compact_state,
            )
        self.assertTrue(result["a"]["ok"])
        self.assertFalse(result["b"]["ok"])
        self.assertEqual(errors, 1)

    def test_writer_rotates_and_writes_valid_jsonl(self):
        with tempfile.TemporaryDirectory() as temporary:
            writer = capture.RotatingJsonlWriter(
                Path(temporary), "office-noise", 60
            )
            writer.write({"type": "state"}, 0)
            writer.write({"type": "state"}, 61)
            writer.close()
            self.assertEqual(len(writer.paths), 2)
            records = [
                json.loads(path.read_text(encoding="utf-8").strip())
                for path in writer.paths
            ]
            self.assertEqual(records[0]["filePart"], 1)
            self.assertEqual(records[1]["filePart"], 2)


if __name__ == "__main__":
    unittest.main()

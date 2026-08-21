from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "office_noise_analyze.py"
SPEC = importlib.util.spec_from_file_location("office_noise_analyze", SCRIPT)
analyze = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = analyze
SPEC.loader.exec_module(analyze)


def device(data, latency=10.0):
    return {"ok": True, "latencyMs": latency, "data": data}


def state_data(
    *,
    source,
    present,
    target_count,
    local_target_count=0,
    source_count=1,
    leader="node-a",
):
    return {
        "presence": present,
        "motion": present,
        "targetCount": target_count,
        "targets": (
            [{"x": 1000, "y": 2000, "sourceCount": source_count}]
            if target_count else []
        ),
        "fusion": {
            "authoritative": True,
            "state": "active",
            "source": source,
            "groupTerm": 7,
        },
        "local": {
            "presence": local_target_count > 0,
            "motion": local_target_count > 0,
            "targetCount": local_target_count,
            "targets": (
                [{"x": 100, "y": 200}]
                if local_target_count else []
            ),
        },
        "automation": {
            "available": True,
            "health": "healthy",
            "leaderNodeId": leader,
            "groupTerm": 7,
            "presence": present,
            "motion": present,
            "targetCount": target_count,
        },
        "debug": {
            "staticRadar": {
                "presence": False,
                "moving": False,
                "still": False,
                "assistActive": False,
            }
        },
    }


def record(record_type, captured_at, devices):
    return {
        "schemaVersion": 1,
        "type": record_type,
        "capturedAt": captured_at,
        "devices": devices,
    }


class OfficeNoiseAnalyzeTests(unittest.TestCase):
    def write_session(self, directory, records):
        path = Path(directory) / (
            "office-noise-20260101-000000-000000-part0001.jsonl"
        )
        path.write_text(
            "\n".join(json.dumps(item) for item in records) + "\n",
            encoding="utf-8",
        )
        return path

    def session_record(self):
        return {
            "schemaVersion": 1,
            "type": "session",
            "capturedAt": "2026-01-01T00:00:00+00:00",
            "devices": {
                "a": {"baseUrl": "http://192.0.2.1"},
                "b": {"baseUrl": "http://192.0.2.2"},
            },
            "intervals": {
                "stateSeconds": 1.0,
                "groupsSeconds": 10.0,
                "systemSeconds": 60.0,
            },
        }

    def test_unoccupied_event_uses_local_group_leader_and_attributes_sources(self):
        records = [self.session_record()]
        values = [
            ("00", False, 0, 0, 0, 1),
            ("01", True, 1, 1, 0, 1),
            ("02", True, 1, 1, 1, 2),
            ("03", False, 0, 0, 0, 1),
        ]
        for second, present, targets, local_a, local_b, sources in values:
            records.append(record(
                "state",
                f"2026-01-01T00:00:{second}+00:00",
                {
                    "a": device(state_data(
                        source="local_group_leader",
                        present=present,
                        target_count=targets,
                        local_target_count=local_a,
                        source_count=sources,
                    )),
                    "b": device(state_data(
                        source="group_leader_snapshot",
                        present=False if second == "01" else present,
                        target_count=0 if second == "01" else targets,
                        local_target_count=local_b,
                        source_count=sources,
                    )),
                },
            ))
        label = analyze.LabelInterval(
            "empty",
            "unoccupied",
            analyze.parse_timestamp("2026-01-01T00:00:00+00:00"),
            None,
            0,
        )
        occupied = analyze.LabelInterval(
            "occupied",
            "occupied",
            analyze.parse_timestamp("2026-01-01T00:00:00+00:00"),
            None,
            1,
        )
        with tempfile.TemporaryDirectory() as temporary:
            path = self.write_session(temporary, records)
            report = analyze.CaptureAnalyzer(
                [label, occupied],
                comparison_pairs=[("a", "b")],
            ).analyze([path])

        empty = report["labels"][0]
        occupied_report = report["labels"][1]
        self.assertEqual(report["fusion"]["authorityAliases"], {"a": 4})
        self.assertEqual(report["fusion"]["endpointDisagreementSampleCount"], 1)
        self.assertEqual(empty["falsePresenceEventCount"], 1)
        self.assertEqual(empty["presenceSecondsEstimate"], 2.0)
        self.assertEqual(empty["sourceCountDistribution"], {"1": 1, "2": 1})
        self.assertEqual(
            empty["localTargetSamplesByDevice"],
            {"a": 2, "b": 1},
        )
        self.assertEqual(
            empty["events"][0]["localSourceSamples"],
            {"a": 2, "b": 1},
        )
        self.assertEqual(empty["topCoordinateBins500mm"][0]["x"], 1000)
        self.assertEqual(empty["targetCountMismatchSampleCount"], 2)
        comparison = empty["comparisons"][0]
        self.assertEqual(comparison["bothDetectedSampleCount"], 1)
        self.assertEqual(comparison["leftOnlySampleCount"], 1)
        self.assertEqual(comparison["rightOnlySampleCount"], 0)
        self.assertEqual(comparison["neitherDetectedSampleCount"], 2)
        self.assertEqual(comparison["detectionJaccardRatio"], 0.5)
        self.assertEqual(
            comparison["bothDetectedGroupCorroboratedSampleCount"],
            1,
        )
        local_track = empty["devices"]["a"]["localTrack"]
        self.assertEqual(local_track["episodeCount"], 1)
        self.assertEqual(local_track["singleTargetStepSampleCount"], 1)
        self.assertEqual(local_track["singleTargetStepMaxMm"], 0.0)
        self.assertEqual(occupied_report["missedPresenceSampleCount"], 2)
        self.assertEqual(occupied_report["missedPresenceRatio"], 0.5)

    def test_group_resource_and_request_failures_are_reported(self):
        records = [self.session_record()]
        records.append(record(
            "state",
            "2026-01-01T00:00:00+00:00",
            {
                "a": device(state_data(
                    source="local_group_leader",
                    present=False,
                    target_count=0,
                )),
                "b": {"ok": False, "error": "timeout"},
            },
        ))
        records.append(record(
            "groups",
            "2026-01-01T00:00:01+00:00",
            {
                "a": device({
                    "site": {
                        "leaderNodeId": "node-b",
                        "localLeader": False,
                        "term": 3,
                        "participantsOnline": 1,
                        "participantsExpected": 2,
                        "health": "degraded",
                    },
                    "groups": [{
                        "groupId": "room",
                        "leaderNodeId": "node-a",
                        "localLeader": True,
                        "term": 7,
                        "participantsOnline": 1,
                        "participantsExpected": 2,
                        "health": "degraded",
                    }],
                    "shadowFusion": {"validInputs": 1},
                    "productionFusion": {
                        "authoritative": False,
                        "state": "active",
                    },
                })
            },
        ))
        records.append(record(
            "configuration",
            "2026-01-01T00:00:01.500+00:00",
            {
                "a": device({
                    "configurationOwnerNodeId": "node-c",
                    "localOwner": False,
                    "authorityEpoch": 2,
                    "generation": 8,
                    "rosterRevision": 4,
                    "configurationConsistent": True,
                })
            },
        ))
        for second, free, uptime in (("02", 100000, 100), ("03", 99000, 20)):
            records.append(record(
                "system",
                f"2026-01-01T00:00:{second}+00:00",
                {
                    "a": device({
                        "firmware": {"version": "test", "uptimeSeconds": uptime},
                        "platform": {"nodeId": "node-a"},
                        "memory": {
                            "internalFreeBytes": free,
                            "internalLargestFreeBlockBytes": 60000,
                            "httpdStackMinFreeBytes": 3000,
                        },
                        "wifi": {"rssi": -40},
                    })
                },
            ))
        with tempfile.TemporaryDirectory() as temporary:
            path = self.write_session(temporary, records)
            report = analyze.CaptureAnalyzer([]).analyze([path])

        self.assertEqual(report["capture"]["requestErrors"], {"state": 1})
        self.assertEqual(report["site"]["anomalyCount"], 1)
        self.assertEqual(report["group"]["anomalyCount"], 1)
        self.assertEqual(report["group"]["localLeaderProductionFailureCount"], 1)
        self.assertEqual(report["group"]["validInputs"], {"1": 1})
        self.assertEqual(report["roles"]["configurationOwners"], {"node-c": 1})
        self.assertEqual(report["roles"]["siteLeaders"], {"node-b": 1})
        self.assertEqual(report["roles"]["groupLeaders"], {"node-a": 1})
        self.assertEqual(len(report["roles"]["transitions"]), 3)
        resource = report["resources"]["a"]
        self.assertEqual(resource["rebootCount"], 1)
        self.assertEqual(resource["internalFreeMinBytes"], 99000.0)
        self.assertLess(resource["internalFreeTrendBytesPerHour"], 0)

    def test_online_but_leaderless_roles_are_anomalous(self):
        records = [self.session_record(), record(
            "groups",
            "2026-01-01T00:00:01+00:00",
            {
                "a": device({
                    "site": {
                        "leaderNodeId": "",
                        "localLeader": False,
                        "term": 3,
                        "participantsOnline": 5,
                        "participantsExpected": 5,
                        "quorumRequired": 3,
                        "quorumAvailable": False,
                        "health": "healthy",
                    },
                    "groups": [{
                        "groupId": "room",
                        "leaderNodeId": "",
                        "localLeader": False,
                        "term": 7,
                        "participantsOnline": 5,
                        "participantsExpected": 5,
                        "quorumRequired": 3,
                        "quorumAvailable": False,
                        "canonicalAssignment": False,
                        "credentialsComplete": False,
                        "health": "healthy",
                    }],
                    "shadowFusion": {"validInputs": 0},
                    "productionFusion": {
                        "authoritative": False,
                        "state": "degraded",
                    },
                })
            },
        )]
        with tempfile.TemporaryDirectory() as temporary:
            path = self.write_session(temporary, records)
            report = analyze.CaptureAnalyzer([]).analyze([path])

        self.assertEqual(report["site"]["anomalyCount"], 1)
        self.assertEqual(report["group"]["anomalyCount"], 1)
        self.assertEqual(
            report["site"]["anomalyExamples"][0]["leaderNodeId"],
            "",
        )
        self.assertFalse(
            report["group"]["anomalyExamples"][0]["quorumAvailable"]
        )

    def test_labels_file_rejects_naive_timestamps(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "labels.json"
            path.write_text(json.dumps({
                "schemaVersion": 1,
                "intervals": [{
                    "name": "empty",
                    "expectedState": "unoccupied",
                    "start": "2026-01-01T00:00:00",
                }],
            }), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "UTC offset"):
                analyze.load_labels(path)

    def test_comparison_parser_rejects_invalid_and_duplicate_aliases(self):
        self.assertEqual(analyze.parse_comparison("178,78"), ("178", "78"))
        with self.assertRaisesRegex(ValueError, "different"):
            analyze.parse_comparison("178,178")
        with self.assertRaisesRegex(ValueError, "exactly two"):
            analyze.parse_comparison("178")

    def test_directory_with_multiple_sessions_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "capture-a-part0001.jsonl").write_text("{}\n")
            (root / "capture-b-part0001.jsonl").write_text("{}\n")
            with self.assertRaisesRegex(ValueError, "multiple capture sessions"):
                analyze.resolve_input_files([temporary])


if __name__ == "__main__":
    unittest.main()

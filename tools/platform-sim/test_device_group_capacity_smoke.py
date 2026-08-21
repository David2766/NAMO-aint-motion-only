from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "device_group_capacity_smoke.py"
TOOLS_DIR = SCRIPT.parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))
SPEC = importlib.util.spec_from_file_location("device_group_capacity_smoke", SCRIPT)
capacity = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = capacity
SPEC.loader.exec_module(capacity)


class FakeClient:
    def __init__(self, participants: list[str] | None = None) -> None:
        self.participants = list(participants or [])

    def get(self, path: str):
        assert path == "/api/platform/participants"
        records = [
            {
                "participantId": participant_id,
                "membership": "active",
                "readiness": {"ready": True},
            }
            for participant_id in self.participants
        ]
        return capacity.readiness_smoke.Response(200, {
            "ok": True,
            "registry": {
                "ready": True,
                "remoteCount": len(records),
                "activeRemoteCount": len(records),
            },
            "readiness": {"ready": True},
            "credentials": {"ready": True},
            "participants": records,
        })


class DeviceGroupCapacitySmokeTests(unittest.TestCase):
    def test_provisions_six_remote_records(self) -> None:
        client = FakeClient()
        created: list[str] = []

        def provision(target, participant_id):
            target.participants.append(participant_id)

        with mock.patch.object(
            capacity.readiness_smoke,
            "provision_capacity_participant",
            side_effect=provision,
        ):
            result = capacity.provision_capacity_records(
                client, 6, False, created
            )

        self.assertEqual(result, created)
        self.assertEqual(len(created), 6)
        self.assertEqual(len(set(created)), 6)
        self.assertEqual(len(client.participants), 6)

    def test_existing_records_require_explicit_permission(self) -> None:
        client = FakeClient(["namo-node-existing"])
        with self.assertRaisesRegex(RuntimeError, "already has remote"):
            capacity.provision_capacity_records(client, 6, False, [])

    def test_existing_records_are_topped_up_to_six(self) -> None:
        client = FakeClient([
            "namo-node-1",
            "namo-node-2",
            "namo-node-3",
            "namo-node-4",
        ])
        created: list[str] = []

        def provision(target, participant_id):
            target.participants.append(participant_id)

        with mock.patch.object(
            capacity.readiness_smoke,
            "provision_capacity_participant",
            side_effect=provision,
        ):
            capacity.provision_capacity_records(client, 6, True, created)

        self.assertEqual(len(created), 2)
        self.assertEqual(len(client.participants), 6)

    def test_partial_provisioning_keeps_ids_for_finally_cleanup(self) -> None:
        client = FakeClient()
        created: list[str] = []

        def provision(target, participant_id):
            target.participants.append(participant_id)
            if len(target.participants) == 3:
                raise RuntimeError("injected reboot")

        with mock.patch.object(
            capacity.readiness_smoke,
            "provision_capacity_participant",
            side_effect=provision,
        ):
            with self.assertRaisesRegex(RuntimeError, "injected reboot"):
                capacity.provision_capacity_records(
                    client, 6, False, created
                )

        self.assertEqual(len(created), 3)

    def test_confirmation_is_required_before_persistent_writes(self) -> None:
        args = mock.Mock(
            confirm_persistent_test_records=False,
            remote_participants=6,
        )
        with self.assertRaisesRegex(
            RuntimeError, "confirm-persistent-test-records"
        ):
            capacity.run_capacity_smoke(args, [])


if __name__ == "__main__":
    unittest.main()

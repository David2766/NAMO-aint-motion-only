from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
import urllib.request


SCRIPT = Path(__file__).resolve().parents[1] / "pairing_trace_capture.py"
SPEC = importlib.util.spec_from_file_location("pairing_trace_capture", SCRIPT)
capture = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = capture
SPEC.loader.exec_module(capture)


class PairingTraceCaptureTests(unittest.TestCase):
    def test_parse_devices_assigns_owner_peer_and_rejects_duplicates(self):
        devices = capture.parse_devices(
            "http://192.168.29.218",
            "http://192.168.29.203",
            ["observer=http://192.168.29.104"],
        )
        self.assertEqual([device.alias for device in devices], ["owner", "peer", "observer"])
        with self.assertRaisesRegex(ValueError, "URLs"):
            capture.parse_devices(
                "http://192.168.29.218",
                "peer=http://192.168.29.218",
                [],
            )

    def test_redact_removes_pairing_material_but_keeps_diagnostics(self):
        value = capture.redact({
            "state": "prepared",
            "confirmationCode": "123456",
            "offer": {"nonce": "abc", "publicKey": "def"},
            "credentialsComplete": False,
        })
        self.assertEqual(value["confirmationCode"], "<redacted>")
        self.assertEqual(value["offer"]["nonce"], "<redacted>")
        self.assertEqual(value["offer"]["publicKey"], "<redacted>")
        self.assertFalse(value["credentialsComplete"])

    def test_snapshot_tracker_writes_changes_errors_and_heartbeats(self):
        tracker = capture.SnapshotTracker(heartbeat_seconds=10)
        ok = {"ok": True, "status": 200, "latencyMs": 12, "data": {"state": "idle"}}
        self.assertEqual(tracker.should_write("owner", "/x", ok, 0), (True, "change"))
        self.assertEqual(tracker.should_write("owner", "/x", ok, 1), (False, "unchanged"))
        self.assertEqual(tracker.should_write("owner", "/x", ok, 11), (True, "heartbeat"))
        failed = {"ok": False, "status": 503, "latencyMs": 4, "data": {}}
        self.assertEqual(tracker.should_write("owner", "/x", failed, 12), (True, "change"))
        self.assertTrue(tracker.should_write("owner", "/x", failed, 13)[0])

    def test_frontend_events_are_validated_written_and_summarized(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "timeline.jsonl"
            started = time.monotonic()
            writer = capture.TimelineWriter(path, started)
            state = capture.CaptureState(
                writer,
                "test-token",
                "test-session",
                "http://192.168.29.218",
            )
            event = {
                "schemaVersion": 1,
                "type": "frontend",
                "sessionId": "test-session",
                "phase": "pairing.confirm",
                "state": "end",
                "durationMs": 1250,
                "detail": {"token": "do-not-store", "peer": "203"},
            }
            self.assertTrue(state.accept_frontend(event))
            self.assertFalse(state.accept_frontend({**event, "sessionId": "wrong"}))
            summary = capture.build_summary(
                state,
                {"requestCount": 12, "requestErrors": 1},
                5.0,
                False,
            )
            writer.close()
            record = json.loads(path.read_text(encoding="utf-8").strip())
            self.assertEqual(record["event"]["detail"]["token"], "<redacted>")
            self.assertEqual(summary["frontendEventCount"], 1)
            self.assertEqual(summary["phases"][0]["durationMs"], 1250)

    def test_removal_checkpoints_are_preserved_in_the_summary(self):
        with tempfile.TemporaryDirectory() as temporary:
            writer = capture.TimelineWriter(
                Path(temporary) / "timeline.jsonl", time.monotonic()
            )
            state = capture.CaptureState(
                writer,
                "test-token",
                "test-session",
                "http://192.168.29.218",
            )
            event = {
                "schemaVersion": 1,
                "type": "frontend",
                "sessionId": "test-session",
                "phase": "removal.replication.sample",
                "state": "event",
                "detail": {"requiredCopies": 2, "matchingCopies": 1},
            }
            self.assertTrue(state.accept_frontend(event))
            summary = capture.build_summary(
                state,
                {"requestCount": 3, "requestErrors": 0},
                1.0,
                False,
            )
            writer.close()
            self.assertEqual(summary["removalCheckpoints"], [{
                "phase": "removal.replication.sample",
                "detail": {"requiredCopies": 2, "matchingCopies": 1},
            }])

    def test_http_receiver_accepts_the_matching_session_and_token(self):
        with tempfile.TemporaryDirectory() as temporary:
            writer = capture.TimelineWriter(
                Path(temporary) / "timeline.jsonl", time.monotonic()
            )
            state = capture.CaptureState(
                writer,
                "test-token",
                "test-session",
                "http://192.168.29.218",
            )
            server = capture.ThreadingHTTPServer(
                ("127.0.0.1", 0), capture.TraceRequestHandler
            )
            setattr(server, "capture", state)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                body = json.dumps({
                    "type": "frontend",
                    "sessionId": "test-session",
                    "phase": "setup.preflight",
                    "state": "begin",
                }).encode()
                request = urllib.request.Request(
                    f"http://127.0.0.1:{server.server_address[1]}/events?token=test-token",
                    data=body,
                    method="POST",
                )
                with urllib.request.urlopen(request, timeout=2) as response:
                    self.assertEqual(response.status, 204)
                self.assertEqual(len(state.frontend_events), 1)
            finally:
                server.shutdown()
                server.server_close()
                writer.close()


if __name__ == "__main__":
    unittest.main()

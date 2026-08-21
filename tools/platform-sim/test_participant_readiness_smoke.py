from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "participant_readiness_smoke.py"
ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("participant_readiness_smoke", SCRIPT)
smoke = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = smoke
SPEC.loader.exec_module(smoke)


class FakeClient:
    def __init__(self, participant_id="satellite-smoke-test",
                 transport=False) -> None:
        self.participant_id = participant_id
        self.membership = None
        self.pose = None
        self.calibration = None
        self.actions = []
        self.fail_approve = False
        self.fail_forget_once = False
        self.reject_gets = False
        self.transport = transport
        self.transport_sends = 0
        self.shadow_transport = {
            "supported": True,
            "smokeMode": transport,
            "socketReady": transport,
            "port": smoke.TRANSPORT_SMOKE_PORT if transport else 0,
            "received": 0,
            "authenticated": 0,
            "admitted": 0,
            "rejected": 0,
            "rateLimited": 0,
            "pending": 0,
            "maxProcessUs": 0,
            "lastResult": "idle",
            "lastAdmission": "ok",
        }

    def response(self, status=200, code=None):
        participant = None
        if self.membership is not None:
            ready = self.pose not in (None, 0) and self.calibration not in (None, 0)
            participant = {
                "participantId": self.participant_id,
                "membership": self.membership,
                "credential": {
                    "provisioned": False,
                    "enabled": False,
                    "keyVersion": 0,
                },
                "readiness": {"ready": ready},
            }
        body = {
            "ok": status < 400,
            "registry": {"ready": True},
            "readiness": {"ready": True},
            "credentials": {
                "ready": True,
                "generation": 1,
                "provisionedCount": 0,
            },
            "shadowTransport": dict(self.shadow_transport),
            "participants": [] if participant is None else [participant],
        }
        if code:
            body["error"] = code
            body["errorInfo"] = {"code": code, "severity": "error", "detail": {}}
        return smoke.Response(status, body)

    def get(self, path):
        if self.reject_gets:
            raise TimeoutError("device temporarily unavailable")
        if path == "/api/system/status":
            return smoke.Response(200, {
                "ok": True,
                "platform": {"siteId": "site-smoke"},
            })
        return self.response()

    def send_datagram(self, port, payload):
        self.assert_transport_datagram(port, payload)
        if self.membership is not None:
            raise AssertionError(
                "transport smoke must run after persistent participant cleanup"
            )
        self.transport_sends += 1
        current = self.shadow_transport
        current["received"] += 1
        if self.transport_sends in (1, 2):
            current["authenticated"] += 1
            current["admitted"] += 1
            current["pending"] = 1
            current["maxProcessUs"] = 300
            current["lastResult"] = "ok"
            current["lastAdmission"] = "ok"
        elif self.transport_sends == 3:
            current["authenticated"] += 1
            current["rejected"] += 1
            current["rateLimited"] += 1
            current["lastResult"] = "rate_limited"
        elif self.transport_sends == 4:
            current["rejected"] += 1
            current["lastResult"] = "authentication_failed"
        else:
            current["authenticated"] += 1
            current["rejected"] += 1
            current["lastResult"] = "admission_rejected"
            current["lastAdmission"] = "duplicate_or_reordered"

    def assert_transport_datagram(self, port, payload):
        if not self.transport:
            raise AssertionError("transport is disabled")
        if port != smoke.TRANSPORT_SMOKE_PORT or not isinstance(payload, bytes):
            raise AssertionError("invalid transport datagram")

    def post_form(self, path, data):
        action = data["action"]
        self.actions.append(action)
        if action == "approve":
            if self.fail_approve:
                return self.response(503, "persistence_failed")
            self.membership = "quarantined"
        elif action == "configure":
            spatial = data["pointTracks"] == "true"
            if spatial and (
                    not int(data["geometryVersion"]) or
                    not int(data["localPoseVersion"])):
                return self.response(400, "participant_configuration_required")
            self.pose = int(data["poseVersion"]) if spatial else 1
            self.calibration = int(data["calibrationVersion"]) if spatial else 1
            self.membership = "quarantined"
        elif action == "activate":
            if not self.pose or not self.calibration:
                return self.response(409, "not_ready")
            self.membership = "active"
        elif action in ("quarantine", "revoke") and self.membership is not None:
            self.membership = "quarantined" if action == "quarantine" else "revoked"
        elif action == "forget":
            if self.fail_forget_once:
                self.fail_forget_once = False
                return self.response(503, "persistence_failed")
            self.membership = None
            self.pose = None
            self.calibration = None
        return self.response()


class ParticipantReadinessSmokeTests(unittest.TestCase):
    def test_full_sequence_and_cleanup(self) -> None:
        client = FakeClient()
        smoke.run_smoke(client, "satellite-smoke-test")
        self.assertIsNone(client.membership)

    def test_capacity_participant_remains_active_until_explicit_cleanup(self) -> None:
        client = FakeClient("satellite-smoke-capacity-test")
        smoke.provision_capacity_participant(
            client, "satellite-smoke-capacity-test"
        )
        self.assertEqual(client.membership, "active")
        self.assertTrue(smoke._cleanup(client, "satellite-smoke-capacity-test"))
        self.assertIsNone(client.membership)

    def test_transport_sequence_uses_ram_fixture_without_enrollment(self) -> None:
        client = FakeClient(smoke.TRANSPORT_SMOKE_PARTICIPANT_ID, transport=True)
        with mock.patch.object(smoke.time, "sleep"):
            smoke.run_smoke(
                client,
                smoke.TRANSPORT_SMOKE_PARTICIPANT_ID,
                transport=True,
            )
        self.assertIsNone(client.membership)
        self.assertEqual(client.actions, [])
        self.assertEqual(client.transport_sends, 5)
        self.assertEqual(client.shadow_transport["admitted"], 2)
        self.assertEqual(client.shadow_transport["rateLimited"], 1)

    def test_transport_observation_round_trips_through_host_codec(self) -> None:
        encode = smoke._load_transport_codec()
        from codec import decode_authenticated_observation, participant_token

        observation = smoke._transport_observation(
            "site-smoke", "transport-smoke-test", 7
        )
        packet = encode(observation, smoke.TRANSPORT_SMOKE_KEY)
        decoded = decode_authenticated_observation(
            packet,
            lambda token, version: smoke.TRANSPORT_SMOKE_KEY
            if token == participant_token(smoke.TRANSPORT_SMOKE_PARTICIPANT_ID)
            and version == 1 else None,
        )
        self.assertEqual(decoded, observation)
        self.assertLessEqual(len(packet), 768)

    def test_transport_run_uses_one_fresh_boot_id(self) -> None:
        observations = []

        def encode(observation, _key):
            observations.append(observation)
            return b"transport-packet"

        with mock.patch.object(smoke, "_load_transport_codec",
                               return_value=encode), \
                mock.patch.object(smoke.secrets, "token_hex",
                                  side_effect=["first", "second"]), \
                mock.patch.object(smoke.time, "sleep"):
            smoke._run_transport_smoke(
                FakeClient(smoke.TRANSPORT_SMOKE_PARTICIPANT_ID,
                           transport=True),
                "site-smoke",
            )
            smoke._run_transport_smoke(
                FakeClient(smoke.TRANSPORT_SMOKE_PARTICIPANT_ID,
                           transport=True),
                "site-smoke",
            )

        first_run_ids = {item["bootId"] for item in observations[:5]}
        second_run_ids = {item["bootId"] for item in observations[5:]}
        self.assertEqual(first_run_ids, {"transport-smoke-first"})
        self.assertEqual(second_run_ids, {"transport-smoke-second"})

    def test_failure_reports_api_code_and_closes_pairing_window(self) -> None:
        client = FakeClient()
        client.fail_approve = True
        with self.assertRaisesRegex(
                RuntimeError, "failed to approve participant.*status=503.*persistence_failed"):
            smoke.run_smoke(client, "satellite-smoke-test")
        self.assertIn("close", client.actions)
        self.assertIsNone(client.membership)

    def test_cleanup_stops_after_device_becomes_unreachable(self) -> None:
        client = mock.Mock()
        client.post_form.side_effect = TimeoutError("device rebooted")
        self.assertFalse(smoke._cleanup(client, "satellite-smoke-test"))
        self.assertEqual(client.post_form.call_count, 1)

    def test_cleanup_uses_minimum_transitions_and_retries_transient_forget(self) -> None:
        client = FakeClient()
        client.membership = "active"
        client.pose = 1
        client.calibration = 1
        client.fail_forget_once = True

        self.assertTrue(smoke._cleanup(client, "satellite-smoke-test"))
        self.assertIsNone(client.membership)
        self.assertEqual(
            client.actions,
            ["close", "quarantine", "forget", "forget"],
        )

    def test_success_does_not_repeat_get_after_cleanup_verified_removal(self) -> None:
        client = FakeClient()
        original_cleanup = smoke._cleanup

        def cleanup_then_disconnect(target, participant_id):
            cleaned = original_cleanup(target, participant_id)
            target.reject_gets = True
            return cleaned

        with mock.patch.object(smoke, "_cleanup", side_effect=cleanup_then_disconnect):
            smoke.run_smoke(client, "satellite-smoke-test")

    def test_only_reserved_smoke_participants_are_selected_for_cleanup(self) -> None:
        response = smoke.Response(200, {
            "participants": [
                {"participantId": "satellite-smoke-old"},
                {"participantId": "namo-node-production"},
                {"participantId": None},
            ]
        })
        self.assertEqual(
            smoke._smoke_participant_ids(response),
            ["satellite-smoke-old"],
        )


class HttpRoutingStackRegressionTests(unittest.TestCase):
    def test_request_url_buffer_is_not_held_on_httpd_stack(self) -> None:
        component = ROOT / "components" / "radar_api_server"
        server_source = (component / "radar_api_server.cpp").read_text(encoding="utf-8")
        server_header = (component / "radar_api_server.h").read_text(encoding="utf-8")
        self.assertEqual(
            server_source.count("request->url_to(this->request_url_buffer_)"), 3
        )
        self.assertIn(
            "FirmwareUpdateHandler::can_handle_upload(request, url)",
            server_source,
        )
        self.assertNotIn("char url_buf[AsyncWebServerRequest::URL_BUF_SIZE]", server_source)
        self.assertIn(
            "std::array<char, AsyncWebServerRequest::URL_BUF_SIZE> request_url_buffer_",
            server_header,
        )

        for handler_path in component.glob("*_handler.cpp"):
            handler_source = handler_path.read_text(encoding="utf-8")
            self.assertNotIn(
                "url_to(", handler_source,
                f"{handler_path.name} must reuse the URL parsed by RadarApiServer",
            )
            self.assertNotIn(
                "this->can_handle(request)", handler_source,
                f"{handler_path.name} must not nest route matching inside execution",
            )

    def test_system_status_avoids_large_stack_serialization_buffers(self) -> None:
        component = ROOT / "components" / "radar_api_server"
        system_source = (component / "system_handler.cpp").read_text(encoding="utf-8")
        identity_source = (component / "platform_identity_store.cpp").read_text(
            encoding="utf-8"
        )
        status_body = system_source[
            system_source.index("void SystemHandler::handle_status_") :
        ]

        self.assertNotIn("char json[640]", identity_source)
        self.assertIn("json.reserve(448)", identity_source)
        self.assertNotIn("beginResponseStream", status_body)
        self.assertNotIn("stream->printf(", status_body)
        self.assertNotIn("writer.format(", status_body)
        self.assertGreaterEqual(
            status_body.count("append_status_unsigned("), 8
        )
        self.assertIn("writer.unsigned_number(", status_body)
        self.assertIn("writer.signed_number(", status_body)
        self.assertIn('writer.write("{\\\"ok\\\":true");', status_body)
        self.assertIn(
            '",\\\"platform\\\":{\\\"schemaVersion\\\":1,'
            '\\\"identityReady\\\":"',
            status_body,
        )
        self.assertIn("writer.finish()", status_body)
        self.assertNotIn('\\\"platform\\\":%s', status_body)

    def test_participant_post_drops_action_frames_before_serializing(self) -> None:
        component = ROOT / "components" / "radar_api_server"
        source = (component / "platform_handler.cpp").read_text(
            encoding="utf-8"
        )
        post_start = source.index("void PlatformHandler::handle_participants_post_")
        dispatch_start = source.index("bool PlatformHandler::execute_participant_action_", post_start)
        post_body = source[post_start:dispatch_start]

        self.assertIn("execute_participant_action_", post_body)
        self.assertIn("send_participants_", post_body)
        self.assertIn('"pre_mutation"', post_body)
        self.assertIn('"post_mutation"', post_body)
        self.assertNotIn("ParticipantReadinessConfig config", post_body)
        self.assertNotIn("parse_bool_arg", post_body)

        configure_start = source.index("bool PlatformHandler::execute_configure_action_", dispatch_start)
        existing_start = source.index(
            "bool PlatformHandler::execute_existing_participant_action_", configure_start
        )
        configure_body = source[configure_start:existing_start]
        self.assertIn(
            "auto &scratch = this->response_workspace_->participant_configure",
            configure_body,
        )
        self.assertNotIn("ParticipantReadinessConfig config{}", configure_body)
        self.assertIn('parse_optional_bool_arg(request, "automaticGroup"', configure_body)
        self.assertIn("local_configuration_owner", configure_body)
        self.assertIn("resolve_automatic_spatial_group", configure_body)
        self.assertIn("config.space_id, config.group_id", configure_body)
        self.assertNotIn("send_participants_", configure_body)
        self.assertIn('writer.write(",\\\"directAssignment\\\":")', source)
        self.assertIn("profile->config.local_pose_version", source)

        server_source = (
            ROOT / "components" / "radar_api_server" / "radar_api_server.cpp"
        ).read_text(encoding="utf-8")
        refresh_start = server_source.index("bool RadarApiServer::refresh_owned_site_roster(")
        refresh_end = server_source.index(
            "bool RadarApiServer::reconcile_owned_spatial_assignments_", refresh_start
        )
        refresh_body = server_source[refresh_start:refresh_end]
        self.assertLess(
            refresh_body.index("reconcile_local_spatial_profiles("),
            refresh_body.index("apply_automatic_spatial_group_assignment("),
        )

        stack_guard = (
            component / "httpd_stack_guard.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("NAMO_HTTPD_STACK_SIZE = 8192", stack_guard)

    def test_shadow_transport_is_dev_only_and_streamed_in_bounded_fields(self) -> None:
        component = ROOT / "components" / "radar_api_server"
        server_source = (component / "radar_api_server.cpp").read_text(
            encoding="utf-8"
        )
        server_header = (component / "radar_api_server.h").read_text(
            encoding="utf-8"
        )
        platform_source = (component / "platform_handler.cpp").read_text(
            encoding="utf-8"
        )
        component_config = (component / "__init__.py").read_text(
            encoding="utf-8"
        )
        transport_source = (component / "observation_transport.cpp").read_text(
            encoding="utf-8"
        )
        transport_header = (component / "observation_transport.h").read_text(
            encoding="utf-8"
        )

        self.assertIn("#ifdef NAMO_SHADOW_TRANSPORT_SMOKE", server_source)
        self.assertIn('cg.add_define("NAMO_SHADOW_TRANSPORT_SMOKE")',
                      component_config)
        self.assertIn('\\"shadowTransport\\"', platform_source)
        self.assertIn("request->beginResponseStream", platform_source)
        self.assertIn("ObservationTransport *observation_transport_{nullptr}",
                      server_header)
        self.assertNotIn("ObservationTransport observation_transport_", server_header)
        self.assertIn("ParticipantRecord smoke_participant_", transport_header)
        self.assertIn("ParticipantReadinessRecord smoke_readiness_",
                      transport_header)
        self.assertIn("use_smoke_fixture", transport_source)
        self.assertIn("&this->smoke_participant_", transport_source)

    def test_credential_api_exposes_metadata_but_never_secret_material(self) -> None:
        source = (
            ROOT / "components" / "radar_api_server" / "platform_handler.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn('\\"credentials\\"', source)
        self.assertIn('\\"credential\\"', source)
        self.assertIn('\\"provisioned\\"', source)
        self.assertIn('\\"keyVersion\\"', source)
        self.assertNotIn('\\"participantToken\\"', source)
        self.assertNotIn('\\"transportKey\\"', source)
        self.assertNotIn('\\"keyBytes\\"', source)


if __name__ == "__main__":
    unittest.main()

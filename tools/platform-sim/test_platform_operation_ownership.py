import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
COMPONENT = ROOT / "components" / "radar_api_server"


class PlatformOperationOwnershipTests(unittest.TestCase):
    def test_http_handler_has_no_direct_persistent_enrollment_mutations(self):
        source = (COMPONENT / "platform_handler.cpp").read_text(encoding="utf-8")
        direct_calls = (
            "approve_candidate(",
            "configure_participant(",
            "reconfigure_active_participant(",
            ".activate(",
            ".quarantine(",
            ".revoke(",
            ".forget(",
            "set_community_leader_eligible(",
        )
        for call in direct_calls:
            self.assertNotIn(call, source)
        self.assertIn("transient_enrollment_action", source)
        self.assertIn("participant_operation().try_acquire()", source)

    def test_main_loop_automatic_invalidation_uses_the_operation_boundary(self):
        source = (COMPONENT / "radar_api_server.cpp").read_text(encoding="utf-8")
        function = re.search(
            r"void RadarApiServer::process_spatial_readiness_invalidation_\(.*?\n}\n",
            source,
            re.DOTALL,
        )
        self.assertIsNotNone(function)
        body = function.group(0)
        self.assertIn("PlatformParticipantOperationAction::INVALIDATE_READINESS", body)
        self.assertIn("participant_operation_.execute", body)
        self.assertNotIn("participant_enrollment_.configure_participant", body)

    def test_pairing_commit_and_http_mutations_share_one_lease(self):
        source = (COMPONENT / "radar_api_server.cpp").read_text(encoding="utf-8")
        process = re.search(
            r"ParticipantPairingPendingCommit pending_commit\{\};.*?complete_commit\(result, now_ms\);",
            source,
            re.DOTALL,
        )
        self.assertIsNotNone(process)
        prefix = source[max(0, process.start() - 180):process.start()]
        self.assertIn("participant_operation_.try_acquire()", prefix)

    def test_quarantined_configure_is_staged_until_one_activation_commit(self):
        operation = (COMPONENT / "platform_operation.cpp").read_text(
            encoding="utf-8"
        )
        staged = re.search(
            r"const bool staged_canonical_configure =.*?return outcome;\n  }",
            operation,
            re.DOTALL,
        )
        self.assertIsNotNone(staged)
        staged_body = staged.group(0)
        self.assertIn(
            "request.action == PlatformParticipantOperationAction::CONFIGURE",
            staged_body,
        )
        self.assertIn("local_configuration_owner", staged_body)
        self.assertIn("canonical_member != nullptr", staged_body)
        self.assertIn(
            "canonical_member->membership == ParticipantMembership::QUARANTINED",
            staged_body,
        )
        self.assertNotIn("refresh_owned_site_roster", staged_body)

        refresh_call = re.search(
            r"const bool activation_commit =.*?refresh_owned_site_roster\(.*?\)",
            operation,
            re.DOTALL,
        )
        self.assertIsNotNone(refresh_call)
        refresh_body = refresh_call.group(0)
        self.assertIn(
            "request.action == PlatformParticipantOperationAction::ACTIVATE",
            refresh_body,
        )
        self.assertIn(
            "activation_commit || request.automatic_group",
            refresh_body,
        )
        self.assertIn(
            "request.automatic_group",
            refresh_body,
        )

    def test_spatial_profile_commit_does_not_imply_automatic_group_resolution(self):
        source = (COMPONENT / "radar_api_server.cpp").read_text(
            encoding="utf-8"
        )
        start = source.index("bool RadarApiServer::refresh_owned_site_roster(")
        end = source.index(
            "bool RadarApiServer::reconcile_owned_spatial_assignments_", start
        )
        body = source[start:end]
        self.assertIn("const char *spatial_profile_participant_id", body)
        self.assertIn("bool resolve_automatic_group", body)
        self.assertIn(
            "if (resolve_automatic_group &&\n"
            "      spatial_profile_participant_id != nullptr",
            body,
        )
        self.assertIn(
            "spatial_profile_participant_id != nullptr\n"
            "              ? SpatialProfileReconciliationMode::EXPLICIT_CALIBRATION",
            body,
        )

        handler = (COMPONENT / "platform_handler.cpp").read_text(
            encoding="utf-8"
        )
        activate_start = handler.index(
            'if (std::strcmp(action, "activate") == 0) {'
        )
        activate_end = handler.index(
            '} else if (std::strcmp(action, "quarantine") == 0)',
            activate_start,
        )
        activate_body = handler[activate_start:activate_end]
        self.assertIn(
            'parse_optional_bool_arg(request, "automaticGroup",',
            activate_body,
        )
        self.assertIn("&operation.automatic_group", activate_body)


if __name__ == "__main__":
    unittest.main()

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
COMPONENT = ROOT / "components" / "radar_api_server"


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        token = source[index]
        if token == "{":
            depth += 1
        elif token == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


class PairingBootstrapActivationGlueTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.server = (COMPONENT / "radar_api_server.cpp").read_text(
            encoding="utf-8"
        )
        cls.platform = (COMPONENT / "platform_handler.cpp").read_text(
            encoding="utf-8"
        )
        cls.replication = (COMPONENT / "site_replication.cpp").read_text(
            encoding="utf-8"
        )
        cls.authority = (COMPONENT / "site_authority.cpp").read_text(
            encoding="utf-8"
        )
        cls.heartbeat = (COMPONENT / "site_heartbeat.cpp").read_text(
            encoding="utf-8"
        )

    def test_pairing_commit_preserves_durable_enrollment_without_replacing_floorplan(
        self,
    ) -> None:
        body = function_body(
            self.server,
            "ParticipantPairingCommitResult RadarApiServer::commit_participant_pairing(",
        )
        self.assertIn(
            "else if (local_role == ParticipantPairingRole::LEADER)", body
        )
        configuration_commit = body.index(
            "this->site_configuration_store_->replace_for_enrollment("
        )
        directory_reconcile = body.index(
            '!this->reconcile_site_directory_scope_("pairing_joiner")'
        )
        self.assertLess(configuration_commit, directory_reconcile)

        commit_complete = body.index(
            "this->standalone_site_reconciliation_.mark_not_required();",
            directory_reconcile,
        )
        bootstrap_pending = body[directory_reconcile:commit_complete]
        self.assertIn("this->site_directory_reconciliation_check_ms_ = 0;", bootstrap_pending)
        self.assertIn("this->floorplan_replica_check_ms_ = 0;", bootstrap_pending)
        self.assertNotIn(
            "return ParticipantPairingCommitResult::PERSISTENCE_FAILED;",
            bootstrap_pending,
        )
        self.assertNotIn(
            "this->site_replication_->start(", body[:commit_complete]
        )

    def test_automatic_enrollment_requires_an_exact_existing_floorplan(self) -> None:
        body = function_body(self.server, "void RadarApiServer::loop()")
        selection = body.index("select_enrollment_replication_candidate(")
        floorplan_gate = body.index(
            "automatic_enrollment_floorplan_replication_allowed(", selection
        )
        fallback = body.index("select_site_replication_candidate(", floorplan_gate)

        self.assertLess(selection, floorplan_gate)
        self.assertLess(floorplan_gate, fallback)
        self.assertIn(
            "!candidate.available && !enrollment_candidate_available",
            body[floorplan_gate:fallback],
        )
        helper = function_body(
            self.authority,
            "bool automatic_enrollment_floorplan_replication_allowed(",
        )
        self.assertIn("source_endpoint->floorplan_replica_ready", helper)
        self.assertIn(
            "local_config_hash == source_endpoint->floorplan_config_hash", helper
        )
        self.assertIn(
            "local_image_hash == source_endpoint->floorplan_image_hash", helper
        )

        explicit_sync = function_body(
            self.server,
            "SiteReplicationResult RadarApiServer::start_site_replication(",
        )
        self.assertIn("authenticated_enrollment_source_allowed(", explicit_sync)
        self.assertIn(
            "SiteConfigurationReplicationMode::AUTHENTICATED_ENROLLMENT",
            explicit_sync,
        )

    def test_directory_loop_contains_only_bootstrap_and_standard_replication(
        self,
    ) -> None:
        body = function_body(
            self.server,
            "void RadarApiServer::process_automatic_site_directory_replication_(",
        )
        bootstrap = body.index("site_directory_bootstrap_source(")
        normal = body.index("site_directory_replication_required(")
        self.assertLess(bootstrap, normal)
        self.assertNotIn("site_directory_fork_repair_source(", body)
        self.assertIn("SiteDirectoryReplicationMode::INITIAL_BOOTSTRAP", body)
        self.assertIn("SiteDirectoryReplicationMode::STANDARD", body)
        self.assertIn("this->site_authority_snapshot(now_ms).consistent()", body)
        self.assertNotIn("site_directory_fork_repair_source(", self.replication)
        self.assertNotIn("SiteDirectoryReplicationMode::AUTHORITY_REPAIR", self.replication)

    def test_replication_revalidates_the_selected_source_before_commit(self) -> None:
        body = function_body(
            self.replication,
            "void SiteReplicationTransport::advance_(",
        )
        validation = body.index("bool source_valid = false;")
        committing = body.index(
            "SiteReplicationState::DIRECTORY_COMMITTING", validation
        )
        self.assertLess(validation, committing)
        self.assertIn("site_directory_bootstrap_source(", body[:committing])
        self.assertNotIn("site_directory_fork_repair_source(", body[:committing])
        self.assertIn("SiteDirectoryReplicationMode::INITIAL_BOOTSTRAP", body[:committing])
        self.assertNotIn("SiteDirectoryReplicationMode::AUTHORITY_REPAIR", body[:committing])
        self.assertIn("this->fail_(SiteReplicationResult::INVALID_OWNER);", body[:committing])

    def test_activation_uses_replica_admission_before_mutation(
        self,
    ) -> None:
        body = function_body(
            self.platform,
            "bool PlatformHandler::execute_existing_participant_action_(",
        )
        admission = body.index("validate_namo_node_activation(")
        mutation = body.index("this->server_->participant_operation().execute(")
        self.assertLess(admission, mutation)
        self.assertNotIn("canonical_site_directory_ready()", body)
        self.assertNotIn("site_directory_payload_hash(local_directory)", body)
        self.assertIn("site_activation_admission_result_to_string(admission)", body)

    def test_shared_write_gate_distinguishes_no_opt_in_from_unready_successor(
        self,
    ) -> None:
        body = function_body(
            self.server,
            "bool RadarApiServer::shared_site_configuration_writable(",
        )
        runtime_successor = body.index(
            "const auto *successor = this->automatic_configuration_successor(now_ms);"
        )
        configured_successor = body.index(
            "radar_api_server::automatic_configuration_successor(",
            runtime_successor,
        )
        no_opt_in_exception = body.index(
            "!configured_successor_exists", configured_successor
        )
        runtime_readiness = body.index(
            "ParticipantEndpointCache::is_online", no_opt_in_exception
        )

        self.assertLess(runtime_successor, configured_successor)
        self.assertLess(configured_successor, no_opt_in_exception)
        self.assertLess(no_opt_in_exception, runtime_readiness)
        self.assertIn("!this->site_authority_snapshot(now_ms).consistent()", body)

    def test_compatibility_mirror_is_never_advertised_as_canonical(self) -> None:
        body = function_body(
            self.heartbeat,
            "void SiteHeartbeatTransport::send_(",
        )
        self.assertIn("!directory->compatibility_mirror", body)
        self.assertIn(
            "message.site_directory_revision = directory->topology_revision;",
            body,
        )


if __name__ == "__main__":
    unittest.main()

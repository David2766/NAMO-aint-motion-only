from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "device_resource_smoke.py"
ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("device_resource_smoke", SCRIPT)
smoke = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = smoke
SPEC.loader.exec_module(smoke)


def status_fixture(**memory_overrides):
    memory = {
        "internalFreeBytes": 76 * 1024,
        "internalMinFreeBytes": 48 * 1024,
        "internalLargestFreeBlockBytes": 32 * 1024,
        "httpdStackMinFreeBytes": 5 * 1024,
        "httpdStackLowestEndpoint": "/api/platform/groups",
        "httpdStackLowestPhase": "handler",
        "httpdStackLowestEndpointFreeBytes": 5 * 1024,
        "internalMinFreeSource": "observation_fusion",
        "internalMinFreePhase": "cycle",
        "internalMinFreeObservedAtMs": 110000,
        "internalMinFreeEventCount": 4,
        "internalMinLargestFreeBlockBytes": 40 * 1024,
        "internalThresholdSource": "observation_send",
        "internalThresholdPhase": "socket",
        "internalThresholdFreeBytes": 47 * 1024,
        "internalThresholdObservedAtMs": 109500,
        "psramTotal": 8 * 1024 * 1024,
        "psramFree": 7 * 1024 * 1024,
        **memory_overrides,
    }
    return {
        "ok": True,
        "firmware": {"version": "0.5.5-test", "uptimeSeconds": 120},
        "memory": memory,
        "flash": {
            "firmwareUsedBytes": 1900 * 1024,
            "firmwareSlotBytes": 3 * 1024 * 1024,
            "storageUsedBytes": 64 * 1024,
            "storageTotalBytes": 1984 * 1024,
        },
    }


class FakeClient:
    def __init__(self, statuses=None, fail_path=None):
        self.statuses = list(statuses or [status_fixture()])
        self.fail_path = fail_path
        self.paths = []

    def get_json(self, path):
        self.paths.append(path)
        if path == self.fail_path:
            raise TimeoutError("simulated timeout")
        if path == "/api/system/status":
            if len(self.statuses) > 1:
                return self.statuses.pop(0)
            return self.statuses[0]
        return {"ok": True}


class DeviceResourceSmokeTests(unittest.TestCase):
    def test_parser_requires_new_stack_and_largest_block_metrics(self) -> None:
        status = status_fixture()
        del status["memory"]["httpdStackMinFreeBytes"]
        with self.assertRaisesRegex(ValueError, "httpdStackMinFreeBytes"):
            smoke.parse_resource_snapshot(status)

    def test_default_limits_accept_current_target_baseline(self) -> None:
        sample = smoke.parse_resource_snapshot(status_fixture())
        report = smoke.evaluate_resource_samples([sample], smoke.ResourceLimits())
        self.assertTrue(report["passed"])
        self.assertTrue(all(check["passed"] for check in report["checks"]))

    def test_low_httpd_stack_fails_the_verdict(self) -> None:
        sample = smoke.parse_resource_snapshot(
            status_fixture(httpdStackMinFreeBytes=1024)
        )
        report = smoke.evaluate_resource_samples([sample], smoke.ResourceLimits())
        check = next(
            item for item in report["checks"]
            if item["name"] == "httpd_stack_min_free"
        )
        self.assertFalse(check["passed"])
        self.assertFalse(report["passed"])

    def test_lowest_httpd_endpoint_is_preserved_in_report(self) -> None:
        sample = smoke.parse_resource_snapshot(
            status_fixture(
                httpdStackMinFreeBytes=1024,
                httpdStackLowestEndpoint="/api/platform/groups",
                httpdStackLowestPhase="handler",
                httpdStackLowestEndpointFreeBytes=1024,
            )
        )
        report = smoke.evaluate_resource_samples(
            [sample], smoke.ResourceLimits()
        )
        self.assertEqual(
            report["observed"]["httpdStackLowestEndpoint"],
            "/api/platform/groups",
        )
        self.assertEqual(
            report["observed"]["httpdStackLowestPhase"], "handler"
        )
        self.assertEqual(
            report["observed"]["httpdStackLowestEndpointFreeBytes"], 1024
        )

    def test_legacy_status_without_endpoint_attribution_remains_valid(self) -> None:
        status = status_fixture()
        del status["memory"]["httpdStackLowestEndpoint"]
        del status["memory"]["httpdStackLowestPhase"]
        del status["memory"]["httpdStackLowestEndpointFreeBytes"]
        del status["memory"]["internalThresholdSource"]
        del status["memory"]["internalThresholdPhase"]
        del status["memory"]["internalThresholdFreeBytes"]
        del status["memory"]["internalThresholdObservedAtMs"]
        sample = smoke.parse_resource_snapshot(status)
        self.assertIsNone(sample.httpd_stack_lowest_endpoint)
        self.assertIsNone(sample.httpd_stack_lowest_phase)
        self.assertIsNone(sample.httpd_stack_lowest_endpoint_free_bytes)
        self.assertIsNone(sample.internal_threshold_source)
        self.assertIsNone(sample.internal_threshold_phase)
        self.assertIsNone(sample.internal_threshold_free_bytes)
        self.assertIsNone(sample.internal_threshold_observed_at_ms)

    def test_first_48k_crossing_is_preserved_in_report(self) -> None:
        sample = smoke.parse_resource_snapshot(status_fixture())
        report = smoke.evaluate_resource_samples(
            [sample], smoke.ResourceLimits()
        )
        self.assertEqual(
            report["observed"]["internalThresholdSource"],
            "observation_send",
        )
        self.assertEqual(
            report["observed"]["internalThresholdPhase"], "socket"
        )
        self.assertEqual(
            report["observed"]["internalThresholdFreeBytes"], 47 * 1024
        )
        self.assertEqual(
            report["observed"]["internalThresholdObservedAtMs"], 109500
        )

    def test_large_internal_heap_drop_fails_even_when_lifetime_minimum_passes(self) -> None:
        before = smoke.parse_resource_snapshot(status_fixture(internalFreeBytes=80 * 1024))
        after = smoke.parse_resource_snapshot(status_fixture(internalFreeBytes=40 * 1024))
        report = smoke.evaluate_resource_samples([before, after], smoke.ResourceLimits())
        check = next(
            item for item in report["checks"]
            if item["name"] == "internal_free_drop"
        )
        self.assertEqual(check["value"], 40 * 1024)
        self.assertFalse(check["passed"])

    def test_read_only_exercise_is_bounded_and_sampled_after_each_round(self) -> None:
        client = FakeClient()
        with mock.patch.object(smoke.time, "sleep"):
            report = smoke.run_smoke(
                client,
                smoke.ResourceLimits(),
                rounds=2,
                concurrency=3,
                interval_seconds=0.1,
            )
        self.assertTrue(report["passed"])
        self.assertEqual(report["sampleCount"], 3)
        self.assertEqual(report["exercise"]["readRequests"], 12)
        self.assertEqual(client.paths.count("/api/system/status"), 3)
        for endpoint in smoke.READ_ONLY_ENDPOINTS:
            self.assertEqual(client.paths.count(endpoint), 2)

    def test_read_only_endpoint_failure_is_reported_without_losing_samples(self) -> None:
        client = FakeClient(fail_path="/api/platform/groups")
        report = smoke.run_smoke(
            client,
            smoke.ResourceLimits(),
            rounds=1,
            interval_seconds=0,
        )
        self.assertFalse(report["passed"])
        self.assertEqual(report["sampleCount"], 2)
        self.assertEqual(report["observed"]["requestErrorCount"], 1)
        self.assertEqual(
            report["requestErrors"][0]["endpoint"],
            "/api/platform/groups",
        )

    def test_window_scope_ignores_unchanged_preexisting_lifetime_low(self) -> None:
        before = smoke.parse_resource_snapshot(status_fixture(
            internalFreeBytes=76 * 1024,
            internalMinFreeBytes=40 * 1024,
        ))
        after = smoke.parse_resource_snapshot(status_fixture(
            internalFreeBytes=72 * 1024,
            internalMinFreeBytes=40 * 1024,
        ))
        report = smoke.evaluate_resource_samples(
            [before, after],
            smoke.ResourceLimits(min_internal_free_bytes=48 * 1024),
            scope="window",
        )
        self.assertTrue(report["passed"])
        self.assertFalse(report["observed"]["newLifetimeLow"])
        self.assertEqual(
            report["observed"]["windowInternalMinFreeBytes"],
            72 * 1024,
        )

    def test_window_scope_fails_on_a_new_lifetime_low(self) -> None:
        before = smoke.parse_resource_snapshot(status_fixture(
            internalFreeBytes=76 * 1024,
            internalMinFreeBytes=64 * 1024,
        ))
        after = smoke.parse_resource_snapshot(status_fixture(
            internalFreeBytes=72 * 1024,
            internalMinFreeBytes=40 * 1024,
        ))
        report = smoke.evaluate_resource_samples(
            [before, after],
            smoke.ResourceLimits(min_internal_free_bytes=48 * 1024),
            scope="window",
        )
        self.assertFalse(report["passed"])
        self.assertTrue(report["observed"]["newLifetimeLow"])
        self.assertEqual(
            report["observed"]["windowInternalMinFreeBytes"],
            40 * 1024,
        )

    def test_uptime_regression_fails_as_an_unexpected_reboot(self) -> None:
        before_status = status_fixture()
        before_status["firmware"]["uptimeSeconds"] = 120
        after_status = status_fixture()
        after_status["firmware"]["uptimeSeconds"] = 3
        report = smoke.evaluate_resource_samples([
            smoke.parse_resource_snapshot(before_status),
            smoke.parse_resource_snapshot(after_status),
        ], smoke.ResourceLimits())
        self.assertFalse(report["passed"])
        self.assertTrue(report["observed"]["unexpectedReboot"])

    def test_group_roster_empty_migration_retries_without_reboot(self) -> None:
        source = (
            ROOT / "components" / "radar_api_server" /
            "radar_api_server.cpp"
        ).read_text(encoding="utf-8")
        reconcile_start = source.index(
            "bool RadarApiServer::reconcile_group_roster_"
        )
        reconcile_end = source.index(
            "bool RadarApiServer::disable_participant_credential",
            reconcile_start,
        )
        reconcile = source[reconcile_start:reconcile_end]
        self.assertIn(
            "this->group_roster_store_result_ == "
            "GroupRosterStoreResult::EMPTY",
            reconcile,
        )
        self.assertIn("const bool became_ready", reconcile)

        loop_start = source.index("void RadarApiServer::loop()")
        loop = source[loop_start:]
        self.assertIn(
            'this->reconcile_group_roster_("coordinator")', loop
        )
        self.assertIn(
            "if (group_roster_changed && this->group_roster_ready())",
            loop,
        )

    def test_firmware_measurement_uses_constant_size_streamed_fields(self) -> None:
        source = (
            ROOT / "components" / "radar_api_server" / "system_handler.cpp"
        ).read_text(encoding="utf-8")
        server_source = (
            ROOT / "components" / "radar_api_server" / "radar_api_server.cpp"
        ).read_text(encoding="utf-8")
        server_header = (
            ROOT / "components" / "radar_api_server" / "radar_api_server.h"
        ).read_text(encoding="utf-8")
        self.assertIn("heap_caps_get_largest_free_block", source)
        self.assertIn("uxTaskGetStackHighWaterMark(nullptr)", source)
        self.assertIn("internalLargestFreeBlockBytes", source)
        self.assertIn("httpdStackMinFreeBytes", source)
        self.assertIn("httpdStackLowestEndpoint", source)
        self.assertIn("httpdStackLowestPhase", source)
        self.assertIn("httpdStackLowestEndpointFreeBytes", source)
        self.assertIn("internalMinFreeSource", source)
        self.assertIn("internalMinFreeObservedAtMs", source)
        self.assertIn("internalThresholdSource", source)
        self.assertIn("internalThresholdFreeBytes", source)
        self.assertNotIn("char resource_json", source)
        self.assertIn(
            'this->record_httpd_stack_watermark(request_endpoint, "entry");',
            server_source,
        )
        self.assertIn(
            'const char *httpd_stack_lowest_endpoint_{"/unobserved"}',
            server_header,
        )
        self.assertIn(
            'const char *httpd_stack_lowest_phase_{"unobserved"}',
            server_header,
        )
        self.assertNotIn("std::string httpd_stack_lowest", server_header)
        self.assertIn(
            "InternalRamWatermarkDiagnostics internal_ram_watermark",
            (ROOT / "components" / "radar_api_server" /
             "platform_runtime_workspace.h").read_text(encoding="utf-8"),
        )

    def test_first_48k_crossing_uses_bounded_transport_checkpoints(self) -> None:
        workspace = (
            ROOT / "components" / "radar_api_server" /
            "platform_runtime_workspace.h"
        ).read_text(encoding="utf-8")
        server = (
            ROOT / "components" / "radar_api_server" /
            "radar_api_server.cpp"
        ).read_text(encoding="utf-8")
        observation = (
            ROOT / "components" / "radar_api_server" /
            "observation_transport.cpp"
        ).read_text(encoding="utf-8")
        heartbeat = (
            ROOT / "components" / "radar_api_server" /
            "site_heartbeat.cpp"
        ).read_text(encoding="utf-8")

        self.assertIn("bool threshold_recorded", workspace)
        self.assertIn("const char *threshold_source", workspace)
        self.assertIn("minimum_free < 48U * 1024U", server)
        self.assertIn(
            "needed = !diagnostics.threshold_recorded", server
        )
        for checkpoint in (
            '("observation_send", "encoded")',
            '("observation_send", "hmac")',
            '("observation_send", "socket")',
            '("fusion_snapshot_send", "hmac")',
            '("observation_packet", "hmac")',
        ):
            self.assertIn(checkpoint, observation)
        for checkpoint in (
            '("site_heartbeat_send", "hmac")',
            '("site_heartbeat_send", "socket")',
            '("site_heartbeat_packet", "hmac")',
        ):
            self.assertIn(checkpoint, heartbeat)

    def test_httpd_attribution_classifies_dashboard_and_api_families(self) -> None:
        source = (
            ROOT / "components" / "radar_api_server" / "radar_api_server.cpp"
        ).read_text(encoding="utf-8")
        for route in (
            'return "/dashboard";',
            'return "/dashboard/assets/dashboard.js";',
            'return "/dashboard/assets/dashboard.css";',
            'return "/setup";',
            'return "/api/system";',
            'return "/api/config";',
            'return "/api/floorplan";',
            'return "/api/stats";',
            'return "/api/diagnostics";',
            'return "/api/control";',
            'return "/other";',
        ):
            self.assertIn(route, source)
        self.assertNotIn('return "unknown";', source)

    def test_system_status_uses_psram_chunk_workspace_and_cached_metadata(self) -> None:
        component = ROOT / "components" / "radar_api_server"
        source = (component / "system_handler.cpp").read_text(encoding="utf-8")
        header = (component / "system_handler.h").read_text(encoding="utf-8")
        server = (component / "radar_api_server.cpp").read_text(encoding="utf-8")
        status_start = source.index(
            "void SystemHandler::handle_status_"
        )
        status_source = source[status_start:]

        self.assertIn("MALLOC_CAP_SPIRAM", source)
        self.assertIn("CHUNK_SIZE = 768", source)
        self.assertIn(
            "SystemStatusWorkspace *status_workspace_", header
        )
        self.assertIn("this->system_handler_.setup();", server)
        self.assertLess(
            server.index("this->system_handler_.setup();"),
            server.index("this->base_->add_handler(this);"),
        )
        self.assertIn("httpd_resp_send_chunk", source)
        self.assertIn("StatusChunkWriter writer", status_source)
        self.assertIn("std::size_t used_{0}", source)
        self.assertIn("bool flush_()", source)
        self.assertNotIn("std::vsnprintf", source)
        self.assertIn("wifi_ap_record_t wifi_ap", source)
        self.assertIn("RadarStorageStatus storage_status", source)
        self.assertNotIn("writer.format", status_source)
        self.assertNotIn("struct WifiStatus", status_source)
        self.assertNotIn("beginResponseStream", status_source)
        self.assertNotIn("esp_image_get_metadata", status_source)
        setup_start = source.index("void SystemHandler::setup()")
        self.assertIn(
            "esp_image_get_metadata",
            source[setup_start:status_start],
        )

    def test_platform_responses_stream_from_one_psram_chunk(self) -> None:
        component = ROOT / "components" / "radar_api_server"
        source = (component / "platform_handler.cpp").read_text(
            encoding="utf-8"
        )
        header = (component / "platform_handler.h").read_text(
            encoding="utf-8"
        )
        server = (component / "radar_api_server.cpp").read_text(
            encoding="utf-8"
        )
        start = source.index(
            "void PlatformHandler::handle_site_configuration_get_"
        )
        end = source.index(
            "void PlatformHandler::handle_site_configuration_post_", start
        )
        handler = source[start:end]

        self.assertIn("CHUNK_SIZE = 768", source)
        self.assertIn("MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT", source)
        self.assertIn("httpd_resp_send_chunk", source)
        self.assertIn("PlatformJsonChunkWriter writer", handler)
        self.assertNotIn("beginResponseStream", handler)
        setup = source[source.index("void PlatformHandler::setup()"):start]
        self.assertIn("#elif !defined(USE_ESP32)", setup)
        self.assertIn("requires PSRAM; endpoint", setup)
        self.assertIn(
            "PlatformResponseWorkspace *response_workspace_",
            header,
        )
        self.assertIn("this->platform_handler_.setup();", server)
        self.assertLess(
            server.index("this->platform_handler_.setup();"),
            server.index("this->base_->add_handler(this);"),
        )

        participants_start = source.index(
            "void PlatformHandler::send_participants_"
        )
        participants = source[participants_start:]
        self.assertIn("PlatformJsonChunkWriter writer", participants)
        self.assertNotIn("beginResponseStream", participants)
        self.assertNotIn("writer.printf", participants)
        self.assertIn("writer.json_string", participants)
        self.assertIn("writer.unsigned_number", participants)
        self.assertIn("writer.fixed_hex", participants)
        self.assertIn('"diagnostics"', participants)
        self.assertIn('"participant"', participants)
        self.assertIn('"flush"', participants)

    def test_state_response_uses_bounded_psram_double_buffer(self) -> None:
        component = ROOT / "components" / "radar_api_server"
        handler = (component / "state_handler.cpp").read_text(
            encoding="utf-8"
        )
        header = (component / "state_handler.h").read_text(
            encoding="utf-8"
        )
        server = (component / "radar_api_server.cpp").read_text(
            encoding="utf-8"
        )
        server_header = (component / "radar_api_server.h").read_text(
            encoding="utf-8"
        )
        builder = (component / "state_json_builder.cpp").read_text(
            encoding="utf-8"
        )

        self.assertIn("STATE_BUFFER_SIZE = 8192", handler)
        self.assertIn("TRACKER_BUFFER_SIZE = 2048", handler)
        self.assertIn("char state[2][STATE_BUFFER_SIZE]", handler)
        self.assertIn("MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT", handler)
        self.assertIn("httpd_resp_send(raw_request, state", handler)
        self.assertNotIn("beginResponse(200, \"application/json\"", handler)
        self.assertIn("StateUpdateLease begin_update()", header)
        self.assertIn("this->state_handler_.setup();", server)
        self.assertIn("FixedTextBuffer state_json", server)
        self.assertIn("write_debug_json(tracker_json)", server)
        self.assertNotIn("std::string state_json_", server_header)
        self.assertIn("output.clear();", builder)
        self.assertLess(
            server.index("this->state_handler_.setup();"),
            server.index("this->base_->add_handler(this);"),
        )

    def test_stats_response_uses_psram_workspace_and_single_pass_streaming(self) -> None:
        component = ROOT / "components" / "radar_api_server"
        source = (component / "stats_store.cpp").read_text(encoding="utf-8")
        header = (component / "stats_store.h").read_text(encoding="utf-8")
        server = (component / "radar_api_server.cpp").read_text(
            encoding="utf-8"
        )
        write_start = source.index("bool StatsStore::write_json_")
        stream_start = source.index("bool StatsStore::stream_json")
        write_source = source[write_start:stream_start]

        for workspace in (
            "stats_heatmap_cells",
            "stats_response",
            "stats_persist",
        ):
            self.assertIn(f'"{workspace}"', source)
        self.assertIn("MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT", source)
        self.assertIn(
            '#include "esphome/core/defines.h"',
            source,
        )
        self.assertIn("ResponseWorkspace *response_workspace_", header)
        self.assertIn("ResponseWorkspace *persist_workspace_", header)
        self.assertIn("this->stats_store_.setup()", server)
        self.assertLess(
            server.index("this->stats_store_.setup()"),
            server.index("this->base_->add_handler(this);"),
        )
        self.assertIn("SUMMARY_DAYS", write_source)
        self.assertEqual(write_source.count("for (const auto &entry : this->daily_)"), 1)
        self.assertIn("write_heatmap_rle_json", write_source)
        self.assertIn("httpd_resp_send_chunk", source)
        self.assertNotIn("totals_json_", write_source)
        self.assertNotIn("compact_today_heatmap_rle_", write_source)
        self.assertNotIn("std::to_string", write_source)

    def test_httpd_stack_guard_applies_bounded_internal_stack(self) -> None:
        component = ROOT / "components" / "radar_api_server"
        guard = (component / "httpd_stack_guard.cpp").read_text(
            encoding="utf-8"
        )
        codegen = (component / "__init__.py").read_text(encoding="utf-8")

        self.assertIn("NAMO_HTTPD_STACK_SIZE = 8192", guard)
        self.assertIn("httpd_config_t adjusted = *config", guard)
        self.assertIn("__wrap_httpd_start", guard)
        self.assertIn("__real_httpd_start(handle, &adjusted)", guard)
        self.assertIn("region=internal", guard)
        self.assertIn('cg.add_build_flag("-Wl,--wrap=httpd_start")', codegen)

    def test_packet_hmac_uses_fixed_transport_workspaces(self) -> None:
        component = ROOT / "components" / "radar_api_server"
        hmac_header = (component / "fixed_hmac_sha256.h").read_text(
            encoding="utf-8"
        )
        hmac_source = (component / "fixed_hmac_sha256.cpp").read_text(
            encoding="utf-8"
        )
        heartbeat_header = (component / "site_heartbeat.h").read_text(
            encoding="utf-8"
        )
        heartbeat_source = (component / "site_heartbeat.cpp").read_text(
            encoding="utf-8"
        )
        observation_header = (component / "observation_transport.h").read_text(
            encoding="utf-8"
        )
        observation_source = (component / "observation_transport.cpp").read_text(
            encoding="utf-8"
        )

        self.assertIn("mbedtls_sha256_context context_", hmac_header)
        self.assertIn("sizeof(FixedHmacSha256) <= 384", hmac_header)
        self.assertIn("mbedtls_sha256_starts", hmac_source)
        self.assertIn("mbedtls_sha256_update", hmac_source)
        self.assertIn("mbedtls_sha256_finish", hmac_source)
        self.assertNotIn("mbedtls_md_hmac", hmac_source)
        self.assertNotIn("heap_caps_", hmac_source)
        self.assertNotIn("new ", hmac_source)
        self.assertIn("FixedHmacSha256 hmac_{}", heartbeat_header)
        self.assertIn("FixedHmacSha256 hmac_{}", observation_header)
        self.assertIn("this->hmac_.calculate", heartbeat_source)
        self.assertIn("&this->hmac_", observation_source)
        self.assertNotIn("mbedtls_md_hmac", heartbeat_source)
        self.assertNotIn("mbedtls_md_hmac", observation_source)

    def test_fusion_runtime_uses_logged_psram_first_workspaces(self) -> None:
        component = ROOT / "components" / "radar_api_server"
        server = (component / "radar_api_server.cpp").read_text(
            encoding="utf-8"
        )
        server_header = (component / "radar_api_server.h").read_text(
            encoding="utf-8"
        )
        workspace_header = (
            component / "platform_runtime_workspace.h"
        ).read_text(encoding="utf-8")
        operation_header = (component / "platform_operation.h").read_text(
            encoding="utf-8"
        )
        transport = (component / "observation_transport.cpp").read_text(
            encoding="utf-8"
        )
        registry_store = (
            component / "participant_registry_store.cpp"
        ).read_text(encoding="utf-8")
        site_store = (component / "site_configuration.cpp").read_text(
            encoding="utf-8"
        )
        site_replication = (component / "site_replication.cpp").read_text(
            encoding="utf-8"
        )
        site_replication_header = (
            component / "site_replication.h"
        ).read_text(encoding="utf-8")
        site_directory = (component / "site_directory.cpp").read_text(
            encoding="utf-8"
        )
        site_group_summary_header = (
            component / "site_group_summary.h"
        ).read_text(encoding="utf-8")
        site_space_summary_header = (
            component / "site_space_summary.h"
        ).read_text(encoding="utf-8")
        site_heartbeat_header = (
            component / "site_heartbeat.h"
        ).read_text(encoding="utf-8")
        fusion_header = (component / "spatial_shadow_fusion.h").read_text(
            encoding="utf-8"
        )
        runtime_header = (component / "spatial_shadow_runtime.h").read_text(
            encoding="utf-8"
        )
        storage_header = (component / "radar_storage.h").read_text(
            encoding="utf-8"
        )
        storage_source = (component / "radar_storage.cpp").read_text(
            encoding="utf-8"
        )
        group_roster_header = (component / "group_roster.h").read_text(
            encoding="utf-8"
        )
        group_roster = (component / "group_roster.cpp").read_text(
            encoding="utf-8"
        )
        platform_handler = (component / "platform_handler.cpp").read_text(
            encoding="utf-8"
        )

        for runtime_type in (
            "PlatformRuntimeWorkspace",
            "SiteHeartbeatTransport",
            "SiteReplicationTransport",
            "ObservationTransport",
            "SpatialShadowRuntime",
        ):
            self.assertIn(
                f"allocate_runtime_object<{runtime_type}>",
                server,
            )
        for workspace in (
            "observation_parser",
            "observation_inbox",
            "fusion_snapshot_cache",
        ):
            self.assertIn(f'"{workspace}", "psram"', transport)
        self.assertIn("MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT", server)
        self.assertIn("MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT", transport)
        self.assertIn("Runtime allocation component=%s", server)
        self.assertIn("Runtime allocation component=%s", transport)
        self.assertIn('"psram_failed"', server)
        self.assertIn('"allocation_failed"', server)
        self.assertIn(
            "SpatialShadowRuntimeInput spatial_shadow_input_scratch",
            workspace_header,
        )
        self.assertIn(
            "std::array<ObservationTransportSample,",
            workspace_header,
        )
        self.assertIn(
            "SpatialShadowRuntimeInput::MAX_REMOTES",
            workspace_header,
        )
        self.assertIn(
            "spatial_remote_participant_scratch",
            workspace_header,
        )
        self.assertIn(
            "spatial_remote_readiness_scratch",
            workspace_header,
        )
        self.assertIn(
            "FusionSnapshot fusion_snapshot_scratch", workspace_header
        )
        self.assertIn(
            "SPATIAL_SHADOW_MAX_SOURCES = 7",
            fusion_header,
        )
        self.assertIn(
            "source_scratch_{}",
            runtime_header,
        )
        self.assertIn(
            "ParticipantRegistrySnapshot participant_registry_snapshot",
            workspace_header,
        )
        self.assertIn(
            "SiteConfigurationSnapshot site_configuration_snapshot",
            workspace_header,
        )
        self.assertIn(
            "SiteDirectorySnapshot site_directory_snapshot",
            workspace_header,
        )
        self.assertIn(
            "SiteDirectorySnapshot site_directory_update_scratch",
            workspace_header,
        )
        self.assertIn(
            "SiteGroupSummaryCache site_group_summaries",
            workspace_header,
        )
        self.assertIn(
            "SiteSpaceSummaryCache site_space_summaries",
            workspace_header,
        )
        self.assertIn(
            "SiteFloorplanOwnerCache site_floorplan_owners",
            workspace_header,
        )
        self.assertIn(
            "MAX_GROUPS = SiteDirectoryLimits::MAX_GROUPS",
            site_group_summary_header,
        )
        self.assertIn(
            "sizeof(SiteGroupSummaryCache) <= 2048",
            site_group_summary_header,
        )
        self.assertIn("MAX_SPACES = 8", site_space_summary_header)
        self.assertIn(
            "sizeof(SiteSpaceSummaryCache) <= 3072",
            site_space_summary_header,
        )
        self.assertNotIn("world_x_mm", site_space_summary_header)
        self.assertNotIn("boundary_targets", site_space_summary_header)
        self.assertIn(
            "sizeof(SiteFloorplanOwnerCache) <= 1280",
            (component / "site_directory.h").read_text(encoding="utf-8"),
        )
        self.assertIn(
            "MAX_PACKET_SIZE = 384",
            site_heartbeat_header,
        )
        self.assertIn('\\"siteGroupSummary\\"', platform_handler)
        self.assertIn('\\"siteGroups\\"', platform_handler)
        self.assertIn(
            '\\"route\\":{\\"schemaVersion\\":1', platform_handler
        )
        self.assertIn('\\"spaceId\\":', platform_handler)
        self.assertIn(
            '\\"statePath\\":\\"/api/state\\"', platform_handler
        )
        self.assertIn('\\"validationResult\\":\\"%s\\"', platform_handler)
        self.assertIn('\\"mode\\":\\"%s\\"', platform_handler)
        self.assertIn('\\"siteSpaceSummary\\"', platform_handler)
        self.assertIn('\\"siteSpaces\\"', platform_handler)
        self.assertIn(
            "ParticipantReadinessSnapshot participant_readiness_snapshot",
            workspace_header,
        )
        self.assertIn(
            "PlatformParticipantOperationWorkspace participant_operation_workspace",
            workspace_header,
        )
        self.assertIn(
            "PLATFORM_RUNTIME_WORKSPACE_BUDGET = 44U * 1024U",
            workspace_header,
        )
        self.assertIn(
            "PLATFORM_RUNTIME_WORKSPACE_BUDGET",
            workspace_header,
        )
        self.assertIn(
            "sizeof(PlatformParticipantOperationWorkspace) <= 10240",
            operation_header,
        )
        self.assertIn(
            "sizeof(PlatformResponseWorkspace) <= 1024",
            platform_handler,
        )
        self.assertIn(
            "heap_caps_calloc(1, sizeof(ParticipantRegistryStore)",
            registry_store,
        )
        self.assertIn(
            "heap_caps_calloc(1, sizeof(SiteConfigurationStore)",
            site_store,
        )
        self.assertIn(
            "Store allocation component=participant_registry",
            registry_store,
        )
        self.assertIn(
            "Store allocation component=site_configuration",
            site_store,
        )
        self.assertIn(
            "heap_caps_calloc(1, sizeof(SiteDirectoryStore)",
            site_directory,
        )
        self.assertIn(
            "Store allocation component=site_directory",
            site_directory,
        )
        self.assertIn(
            "reason=psram_disabled",
            site_directory,
        )
        self.assertNotIn(
            "new (std::nothrow) SiteDirectoryStore",
            site_directory,
        )
        self.assertIn(
            "prepare_legacy_single_floorplan_projection",
            site_directory,
        )
        self.assertIn(
            "PLATFORM_SECTOR_COUNT = 18",
            storage_header,
        )
        self.assertIn(
            "TAIL_PLATFORM_SECTOR_COUNT = 15",
            storage_header,
        )
        self.assertIn(
            "GROUP_ROSTER_STORAGE_OFFSET = 0x0007E000",
            storage_header,
        )
        self.assertIn(
            "LEGACY_STATS_MAX_SIZE - GROUP_ROSTER_STORAGE_SIZE",
            storage_source,
        )
        self.assertIn(
            "storage->prepare_group_roster_storage()",
            group_roster,
        )
        self.assertIn("GROUP_ROSTER_SLOT_0 = 15", storage_header)
        self.assertIn("GROUP_ROSTER_SLOT_1 = 16", storage_header)
        self.assertIn("GROUP_ROSTER_COMMIT = 17", storage_header)
        self.assertIn(
            "GroupRosterSnapshot group_roster_snapshot", workspace_header
        )
        self.assertIn(
            "GroupRosterSnapshot group_roster_update_scratch",
            workspace_header,
        )
        self.assertNotIn(
            "GroupRosterSnapshot candidate{}", group_roster
        )
        self.assertIn(
            "sizeof(GroupRosterSnapshot) <= 2048", group_roster_header
        )
        self.assertIn(
            "heap_caps_calloc(1, sizeof(GroupRosterStore)", group_roster
        )
        self.assertIn(
            "Store allocation component=group_roster region=psram",
            group_roster,
        )
        self.assertNotIn(
            "new (std::nothrow) GroupRosterStore", group_roster
        )
        self.assertIn(
            '#include "esphome/core/defines.h"',
            site_store,
        )
        self.assertIn(
            "heap_caps_calloc(1, sizeof(SiteReplicationWorkspace)",
            site_replication,
        )
        self.assertIn(
            "Replication workspace allocated in PSRAM",
            site_replication,
        )
        self.assertIn(
            '#include "esphome/core/defines.h"',
            site_replication,
        )
        self.assertIn(
            "reason=psram_disabled",
            site_replication,
        )
        self.assertNotIn(
            "new (std::nothrow) SiteReplicationWorkspace",
            site_replication,
        )
        self.assertGreaterEqual(
            site_replication.count(
                "authenticated_enrollment_source_allowed("
            ),
            2,
        )
        self.assertIn(
            "this->diagnostics_.validation_result = validation",
            site_replication,
        )
        self.assertIn(
            "sizeof(SiteReplicationTransport) <= 2048",
            site_replication_header,
        )
        self.assertIn(
            "SiteReplicationSourceLease source_lease_{}",
            site_replication_header,
        )
        self.assertIn(
            "SITE_REPLICATION_BUSY_PAYLOAD_HASH",
            site_replication_header,
        )
        self.assertIn('\\"source\\":{\\"active\\":', platform_handler)
        self.assertIn(
            "source_count <= SpatialShadowRuntimeInput::MAX_REMOTES",
            server,
        )
        self.assertIn(
            "input.remotes[input.remote_count++]",
            server,
        )
        self.assertNotIn("SiteCoordinator site_coordinator_{}", server_header)
        self.assertNotIn("GroupCoordinator group_coordinator_{}", server_header)
        self.assertNotIn(
            "ParticipantRegistrySnapshot participant_registry_snapshot_{}",
            server_header,
        )
        self.assertNotIn(
            "ParticipantReadinessSnapshot participant_readiness_snapshot_{}",
            server_header,
        )

    def test_system_status_chunked_response_keeps_required_contract_fields(self) -> None:
        source = (
            ROOT / "components" / "radar_api_server" / "system_handler.cpp"
        ).read_text(encoding="utf-8")
        status_source = source[source.index(
            "void SystemHandler::handle_status_"
        ):]
        for field in (
            '\\"device\\"',
            '\\"platform\\"',
            '\\"network\\"',
            '\\"firmware\\"',
            '\\"api\\"',
            '\\"boot\\"',
            '\\"dashboard\\"',
            '\\"schema\\"',
            '\\"memory\\"',
            '\\"flash\\"',
            '\\"storage\\"',
            '\\"wifi\\"',
            '\\"bluetooth\\"',
        ):
            self.assertIn(field, status_source)


if __name__ == "__main__":
    unittest.main()

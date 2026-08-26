#!/usr/bin/env python3
"""Build and run native firmware-logic tests."""

from __future__ import annotations

import os
import argparse
import subprocess
import sys
from pathlib import Path

import build


ROOT = Path(__file__).resolve().parents[3]
NATIVE_DIR = Path(__file__).resolve().parent
TEST_CPP = NATIVE_DIR / "tracker_tests.cpp"
PARTICIPANT_REGISTRY_TEST_CPP = NATIVE_DIR / "participant_registry_tests.cpp"
PARTICIPANT_READINESS_TEST_CPP = NATIVE_DIR / "participant_readiness_tests.cpp"
PARTICIPANT_CREDENTIALS_TEST_CPP = NATIVE_DIR / "participant_credentials_tests.cpp"
PARTICIPANT_PROJECTION_TEST_CPP = NATIVE_DIR / "participant_projection_tests.cpp"
CREDENTIAL_PROVISIONING_TEST_CPP = NATIVE_DIR / "credential_provisioning_tests.cpp"
PARTICIPANT_PAIRING_TEST_CPP = NATIVE_DIR / "participant_pairing_tests.cpp"
PAIRING_BOOTSTRAP_ACTIVATION_TEST_CPP = (
    NATIVE_DIR / "pairing_bootstrap_activation_tests.cpp"
)
PARTICIPANT_DISCOVERY_TEST_CPP = NATIVE_DIR / "participant_discovery_tests.cpp"
PLATFORM_IDENTITY_TEST_CPP = NATIVE_DIR / "platform_identity_tests.cpp"
OBSERVATION_TRANSPORT_TEST_CPP = NATIVE_DIR / "observation_transport_tests.cpp"
GROUP_COORDINATOR_TEST_CPP = NATIVE_DIR / "group_coordinator_tests.cpp"
GROUP_MEMBERSHIP_TEST_CPP = NATIVE_DIR / "group_membership_tests.cpp"
GROUP_ROSTER_TEST_CPP = NATIVE_DIR / "group_roster_tests.cpp"
SITE_COORDINATOR_TEST_CPP = NATIVE_DIR / "site_coordinator_tests.cpp"
SITE_CONFIGURATION_TEST_CPP = NATIVE_DIR / "site_configuration_tests.cpp"
SITE_AUTHORITY_TEST_CPP = NATIVE_DIR / "site_authority_tests.cpp"
SITE_DIRECTORY_TEST_CPP = NATIVE_DIR / "site_directory_tests.cpp"
SITE_GROUP_SUMMARY_TEST_CPP = NATIVE_DIR / "site_group_summary_tests.cpp"
SITE_SPACE_SUMMARY_TEST_CPP = NATIVE_DIR / "site_space_summary_tests.cpp"
SITE_HEARTBEAT_TEST_CPP = NATIVE_DIR / "site_heartbeat_tests.cpp"
SITE_REPLICATION_TEST_CPP = NATIVE_DIR / "site_replication_tests.cpp"
LD2450_STARTUP_GUARD_TEST_CPP = NATIVE_DIR / "ld2450_startup_guard_tests.cpp"
LD245X_STARTUP_SEQUENCE_TEST_CPP = NATIVE_DIR / "ld245x_startup_sequence_tests.cpp"
LD2410_STREAM_HEALTH_TEST_CPP = NATIVE_DIR / "ld2410_stream_health_tests.cpp"
SPATIAL_SHADOW_FUSION_TEST_CPP = NATIVE_DIR / "spatial_shadow_fusion_tests.cpp"
SPATIAL_SHADOW_RUNTIME_TEST_CPP = NATIVE_DIR / "spatial_shadow_runtime_tests.cpp"
SPATIAL_PRODUCTION_FUSION_TEST_CPP = NATIVE_DIR / "spatial_production_fusion_tests.cpp"
ROLE_FAILURE_MATRIX_TEST_CPP = NATIVE_DIR / "role_failure_matrix_tests.cpp"
SHARED_AUTOMATION_AREA_TEST_CPP = NATIVE_DIR / "shared_automation_area_tests.cpp"
FIRMWARE_UPDATE_SECURITY_TEST_CPP = NATIVE_DIR / "firmware_update_security_tests.cpp"
DEVICE_CONFIG_CPP = ROOT / "components" / "radar_api_server" / "device_config_cache.cpp"
SOFTWARE_ZONE_EVIDENCE_CPP = ROOT / "components" / "radar_api_server" / "software_zone_evidence.cpp"
PRESENCE_FUSION_CPP = ROOT / "components" / "radar_api_server" / "presence_fusion.cpp"
SPATIAL_SHADOW_FUSION_CPP = ROOT / "components" / "radar_api_server" / "spatial_shadow_fusion.cpp"
SPATIAL_SHADOW_RUNTIME_CPP = ROOT / "components" / "radar_api_server" / "spatial_shadow_runtime.cpp"
SPATIAL_PRODUCTION_FUSION_CPP = ROOT / "components" / "radar_api_server" / "spatial_production_fusion.cpp"
LD2410_STREAM_HEALTH_CPP = ROOT / "components" / "radar_api_server" / "ld2410_stream_health.cpp"
LOCAL_OBSERVATION_CPP = ROOT / "components" / "radar_api_server" / "local_observation.cpp"
PARTICIPANT_REGISTRY_CPP = ROOT / "components" / "radar_api_server" / "participant_registry.cpp"
PARTICIPANT_REGISTRY_STORE_CPP = ROOT / "components" / "radar_api_server" / "participant_registry_store.cpp"
PARTICIPANT_ENROLLMENT_CPP = ROOT / "components" / "radar_api_server" / "participant_enrollment.cpp"
PARTICIPANT_READINESS_CPP = ROOT / "components" / "radar_api_server" / "participant_readiness.cpp"
SPATIAL_TRANSFORM_CPP = ROOT / "components" / "radar_api_server" / "spatial_transform.cpp"
PARTICIPANT_CREDENTIALS_CPP = ROOT / "components" / "radar_api_server" / "participant_credentials.cpp"
CREDENTIAL_PROVISIONING_CPP = ROOT / "components" / "radar_api_server" / "credential_provisioning.cpp"
PARTICIPANT_PAIRING_CPP = ROOT / "components" / "radar_api_server" / "participant_pairing.cpp"
PARTICIPANT_PAIRING_RUNTIME_CPP = ROOT / "components" / "radar_api_server" / "participant_pairing_runtime.cpp"
PARTICIPANT_DISCOVERY_CPP = ROOT / "components" / "radar_api_server" / "participant_discovery.cpp"
PLATFORM_IDENTITY_CPP = ROOT / "components" / "radar_api_server" / "platform_identity.cpp"
PLATFORM_IDENTITY_STORE_CPP = ROOT / "components" / "radar_api_server" / "platform_identity_store.cpp"
OBSERVATION_TRANSPORT_CPP = ROOT / "components" / "radar_api_server" / "observation_transport.cpp"
FUSION_SNAPSHOT_CPP = ROOT / "components" / "radar_api_server" / "fusion_snapshot.cpp"
FUSION_AUTOMATION_CPP = ROOT / "components" / "radar_api_server" / "fusion_automation.cpp"
GROUP_COORDINATOR_CPP = ROOT / "components" / "radar_api_server" / "group_coordinator.cpp"
GROUP_MEMBERSHIP_CPP = ROOT / "components" / "radar_api_server" / "group_membership.cpp"
GROUP_ROSTER_CPP = ROOT / "components" / "radar_api_server" / "group_roster.cpp"
SITE_COORDINATOR_CPP = ROOT / "components" / "radar_api_server" / "site_coordinator.cpp"
SITE_CONFIGURATION_CPP = ROOT / "components" / "radar_api_server" / "site_configuration.cpp"
SITE_AUTHORITY_CPP = ROOT / "components" / "radar_api_server" / "site_authority.cpp"
SITE_DIRECTORY_CPP = ROOT / "components" / "radar_api_server" / "site_directory.cpp"
SITE_GROUP_SUMMARY_CPP = ROOT / "components" / "radar_api_server" / "site_group_summary.cpp"
SITE_SPACE_SUMMARY_CPP = ROOT / "components" / "radar_api_server" / "site_space_summary.cpp"
SITE_HEARTBEAT_CPP = ROOT / "components" / "radar_api_server" / "site_heartbeat.cpp"
SITE_REPLICATION_CPP = ROOT / "components" / "radar_api_server" / "site_replication.cpp"
SITE_MEMBERSHIP_CPP = ROOT / "components" / "radar_api_server" / "site_membership.cpp"
PRESENCE_REPLAY_LOG_CPP = ROOT / "components" / "radar_api_server" / "presence_replay_log.cpp"
STATE_JSON_BUILDER_CPP = ROOT / "components" / "radar_api_server" / "state_json_builder.cpp"
SHARED_AUTOMATION_AREA_CPP = ROOT / "components" / "radar_api_server" / "shared_automation_area.cpp"
FIRMWARE_UPDATE_SECURITY_CPP = ROOT / "components" / "radar_api_server" / "firmware_update_security.cpp"
FIRMWARE_UPDATE_SESSION_CPP = ROOT / "components" / "radar_api_server" / "firmware_update_session.cpp"
TEST_INCLUDE_DIR = NATIVE_DIR / "include"
OUT_EXE = NATIVE_DIR / ("tracker_tests.exe" if os.name == "nt" else "tracker_tests")


def test_command(compiler: str, kind: str) -> list[str]:
    msvc_args = [
        "cl",
        "/nologo",
        "/std:c++17",
        "/EHsc",
        "/O2",
        f"/I{TEST_INCLUDE_DIR}",
        f"/I{build.INCLUDE_DIR}",
        str(TEST_CPP),
        str(PARTICIPANT_REGISTRY_TEST_CPP),
        str(PARTICIPANT_READINESS_TEST_CPP),
        str(PARTICIPANT_CREDENTIALS_TEST_CPP),
        str(PARTICIPANT_PROJECTION_TEST_CPP),
        str(CREDENTIAL_PROVISIONING_TEST_CPP),
        str(PARTICIPANT_PAIRING_TEST_CPP),
        str(PAIRING_BOOTSTRAP_ACTIVATION_TEST_CPP),
        str(PARTICIPANT_DISCOVERY_TEST_CPP),
        str(PLATFORM_IDENTITY_TEST_CPP),
        str(OBSERVATION_TRANSPORT_TEST_CPP),
        str(GROUP_COORDINATOR_TEST_CPP),
        str(GROUP_MEMBERSHIP_TEST_CPP),
        str(GROUP_ROSTER_TEST_CPP),
        str(SITE_COORDINATOR_TEST_CPP),
        str(SITE_CONFIGURATION_TEST_CPP),
        str(SITE_AUTHORITY_TEST_CPP),
        str(SITE_DIRECTORY_TEST_CPP),
        str(SITE_GROUP_SUMMARY_TEST_CPP),
        str(SITE_SPACE_SUMMARY_TEST_CPP),
        str(SITE_HEARTBEAT_TEST_CPP),
        str(SITE_REPLICATION_TEST_CPP),
        str(LD2450_STARTUP_GUARD_TEST_CPP),
        str(LD245X_STARTUP_SEQUENCE_TEST_CPP),
        str(LD2410_STREAM_HEALTH_TEST_CPP),
        str(SPATIAL_SHADOW_FUSION_TEST_CPP),
        str(SPATIAL_SHADOW_RUNTIME_TEST_CPP),
        str(SPATIAL_PRODUCTION_FUSION_TEST_CPP),
        str(ROLE_FAILURE_MATRIX_TEST_CPP),
        str(SHARED_AUTOMATION_AREA_TEST_CPP),
        str(FIRMWARE_UPDATE_SECURITY_TEST_CPP),
        str(build.TRACKER_CPP),
        str(PRESENCE_FUSION_CPP),
        str(SPATIAL_SHADOW_FUSION_CPP),
        str(SPATIAL_SHADOW_RUNTIME_CPP),
        str(SPATIAL_PRODUCTION_FUSION_CPP),
        str(LD2410_STREAM_HEALTH_CPP),
        str(LOCAL_OBSERVATION_CPP),
        str(PARTICIPANT_REGISTRY_CPP),
        str(PARTICIPANT_REGISTRY_STORE_CPP),
        str(PARTICIPANT_ENROLLMENT_CPP),
        str(PARTICIPANT_READINESS_CPP),
        str(SPATIAL_TRANSFORM_CPP),
        str(PARTICIPANT_CREDENTIALS_CPP),
        str(CREDENTIAL_PROVISIONING_CPP),
        str(PARTICIPANT_PAIRING_CPP),
        str(PARTICIPANT_PAIRING_RUNTIME_CPP),
        str(PARTICIPANT_DISCOVERY_CPP),
        str(PLATFORM_IDENTITY_CPP),
        str(PLATFORM_IDENTITY_STORE_CPP),
        str(OBSERVATION_TRANSPORT_CPP),
        str(FUSION_SNAPSHOT_CPP),
        str(FUSION_AUTOMATION_CPP),
        str(GROUP_COORDINATOR_CPP),
        str(GROUP_MEMBERSHIP_CPP),
        str(GROUP_ROSTER_CPP),
        str(SITE_COORDINATOR_CPP),
        str(SITE_CONFIGURATION_CPP),
        str(SITE_AUTHORITY_CPP),
        str(SITE_DIRECTORY_CPP),
        str(SITE_GROUP_SUMMARY_CPP),
        str(SITE_SPACE_SUMMARY_CPP),
        str(SITE_HEARTBEAT_CPP),
        str(SITE_REPLICATION_CPP),
        str(SITE_MEMBERSHIP_CPP),
        str(PRESENCE_REPLAY_LOG_CPP),
        str(STATE_JSON_BUILDER_CPP),
        str(SHARED_AUTOMATION_AREA_CPP),
        str(FIRMWARE_UPDATE_SECURITY_CPP),
        str(FIRMWARE_UPDATE_SESSION_CPP),
        str(DEVICE_CONFIG_CPP),
        str(SOFTWARE_ZONE_EVIDENCE_CPP),
        f"/Fe:{OUT_EXE}",
    ]

    if kind == "msvc":
        return [compiler, *msvc_args[1:]]

    if kind == "msvc-vsdevcmd":
        batch = NATIVE_DIR / "_test_tracker.cmd"
        compile_command = " ".join(build.cmd_quote(arg) for arg in msvc_args)
        batch.write_text(
            "\n".join(
                [
                    "@echo off",
                    f'call "{compiler}" -arch=x64 -host_arch=x64',
                    "if errorlevel 1 exit /b %errorlevel%",
                    compile_command,
                    "exit /b %errorlevel%",
                    "",
                ]
            ),
            encoding="utf-8",
        )
        return ["cmd", "/c", str(batch)]

    return [
        compiler,
        "-std=c++17",
        "-O2",
        f"-I{TEST_INCLUDE_DIR}",
        f"-I{build.INCLUDE_DIR}",
        str(TEST_CPP),
        str(PARTICIPANT_REGISTRY_TEST_CPP),
        str(PARTICIPANT_READINESS_TEST_CPP),
        str(PARTICIPANT_CREDENTIALS_TEST_CPP),
        str(CREDENTIAL_PROVISIONING_TEST_CPP),
        str(PARTICIPANT_PAIRING_TEST_CPP),
        str(PAIRING_BOOTSTRAP_ACTIVATION_TEST_CPP),
        str(PARTICIPANT_DISCOVERY_TEST_CPP),
        str(PLATFORM_IDENTITY_TEST_CPP),
        str(OBSERVATION_TRANSPORT_TEST_CPP),
        str(GROUP_COORDINATOR_TEST_CPP),
        str(GROUP_MEMBERSHIP_TEST_CPP),
        str(GROUP_ROSTER_TEST_CPP),
        str(SITE_COORDINATOR_TEST_CPP),
        str(SITE_CONFIGURATION_TEST_CPP),
        str(SITE_AUTHORITY_TEST_CPP),
        str(SITE_DIRECTORY_TEST_CPP),
        str(SITE_GROUP_SUMMARY_TEST_CPP),
        str(SITE_SPACE_SUMMARY_TEST_CPP),
        str(SITE_HEARTBEAT_TEST_CPP),
        str(SITE_REPLICATION_TEST_CPP),
        str(LD2450_STARTUP_GUARD_TEST_CPP),
        str(LD245X_STARTUP_SEQUENCE_TEST_CPP),
        str(LD2410_STREAM_HEALTH_TEST_CPP),
        str(SPATIAL_SHADOW_FUSION_TEST_CPP),
        str(SPATIAL_SHADOW_RUNTIME_TEST_CPP),
        str(SPATIAL_PRODUCTION_FUSION_TEST_CPP),
        str(ROLE_FAILURE_MATRIX_TEST_CPP),
        str(SHARED_AUTOMATION_AREA_TEST_CPP),
        str(FIRMWARE_UPDATE_SECURITY_TEST_CPP),
        str(build.TRACKER_CPP),
        str(PRESENCE_FUSION_CPP),
        str(SPATIAL_SHADOW_FUSION_CPP),
        str(SPATIAL_SHADOW_RUNTIME_CPP),
        str(SPATIAL_PRODUCTION_FUSION_CPP),
        str(LD2410_STREAM_HEALTH_CPP),
        str(LOCAL_OBSERVATION_CPP),
        str(PARTICIPANT_REGISTRY_CPP),
        str(PARTICIPANT_REGISTRY_STORE_CPP),
        str(PARTICIPANT_ENROLLMENT_CPP),
        str(PARTICIPANT_READINESS_CPP),
        str(SPATIAL_TRANSFORM_CPP),
        str(PARTICIPANT_CREDENTIALS_CPP),
        str(CREDENTIAL_PROVISIONING_CPP),
        str(PARTICIPANT_PAIRING_CPP),
        str(PARTICIPANT_PAIRING_RUNTIME_CPP),
        str(PARTICIPANT_DISCOVERY_CPP),
        str(PLATFORM_IDENTITY_CPP),
        str(PLATFORM_IDENTITY_STORE_CPP),
        str(OBSERVATION_TRANSPORT_CPP),
        str(FUSION_SNAPSHOT_CPP),
        str(FUSION_AUTOMATION_CPP),
        str(GROUP_COORDINATOR_CPP),
        str(GROUP_MEMBERSHIP_CPP),
        str(GROUP_ROSTER_CPP),
        str(SITE_COORDINATOR_CPP),
        str(SITE_CONFIGURATION_CPP),
        str(SITE_AUTHORITY_CPP),
        str(SITE_DIRECTORY_CPP),
        str(SITE_GROUP_SUMMARY_CPP),
        str(SITE_SPACE_SUMMARY_CPP),
        str(SITE_HEARTBEAT_CPP),
        str(SITE_REPLICATION_CPP),
        str(SITE_MEMBERSHIP_CPP),
        str(PRESENCE_REPLAY_LOG_CPP),
        str(STATE_JSON_BUILDER_CPP),
        str(SHARED_AUTOMATION_AREA_CPP),
        str(FIRMWARE_UPDATE_SECURITY_CPP),
        str(FIRMWARE_UPDATE_SESSION_CPP),
        str(DEVICE_CONFIG_CPP),
        str(SOFTWARE_ZONE_EVIDENCE_CPP),
        "-o",
        str(OUT_EXE),
    ]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check-compiler",
        action="store_true",
        help="Only check whether a supported host C++ compiler is available.",
    )
    parser.add_argument(
        "--skip-if-no-compiler",
        action="store_true",
        help="Exit successfully after reporting a skip when no supported compiler is available.",
    )
    parser.add_argument(
        "--suite",
        choices=(
            "coordinators",
            "firmware",
            "resources",
            "roles",
            "site-heartbeat",
        ),
        help="Run one focused suite after building the native test binary.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    compiler = build.find_compiler()
    if compiler is None:
        if args.skip_if_no_compiler:
            print("tracker_tests: skipped (no host C++ compiler found)")
            return 0
        if args.check_compiler:
            print("tracker_tests: skipped (no host C++ compiler found)")
            return 2
        print(
            "error: no host C++ compiler found. Install Visual Studio Build Tools, MinGW g++, or clang++.",
            file=sys.stderr,
        )
        return 1

    compiler_path, kind = compiler
    if args.check_compiler:
        print(f"tracker_tests: compiler found ({compiler_path})")
        return 0
    command = test_command(compiler_path, kind)
    print(" ".join(command))
    completed = subprocess.run(command, cwd=ROOT)
    if completed.returncode != 0:
        return completed.returncode

    run_command = [str(OUT_EXE)]
    if args.suite is not None:
        run_command.append(args.suite)
    return subprocess.run(run_command, cwd=ROOT).returncode


if __name__ == "__main__":
    raise SystemExit(main())

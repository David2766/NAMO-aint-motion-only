#!/usr/bin/env python3
"""Exercise one ESP32 with six temporary remote spatial records.

The run covers the seven-participant HTTP, persistence, and runtime resource
envelope. It does not synthesize six authenticated observation streams.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import secrets
import sys
import time

import device_resource_smoke as resource_smoke
import participant_readiness_smoke as readiness_smoke


MAX_REMOTE_GROUP_PARTICIPANTS = 6


def _participants(client: readiness_smoke.Client) -> readiness_smoke.Response:
    response = client.get("/api/platform/participants")
    readiness_smoke._require_status(
        response, 200, "participants API is unavailable"
    )
    return response


def _participant_ids(response: readiness_smoke.Response) -> list[str]:
    result: list[str] = []
    for participant in response.body.get("participants", []):
        participant_id = participant.get("participantId")
        if isinstance(participant_id, str) and participant_id:
            result.append(participant_id)
    return result


def _cleanup_with_reconnect(
    client: readiness_smoke.Client,
    participant_id: str,
    attempts: int = 6,
) -> bool:
    for attempt in range(attempts):
        if readiness_smoke._cleanup(client, participant_id):
            return True
        if attempt + 1 < attempts:
            time.sleep(2.0)
    return False


def provision_capacity_records(
    client: readiness_smoke.Client,
    desired_remote_count: int,
    allow_existing: bool,
    created: list[str],
) -> list[str]:
    initial = _participants(client)
    readiness_smoke._cleanup_stale_smoke_participants(client, initial)
    current = _participants(client)
    existing = _participant_ids(current)
    if existing and not allow_existing:
        raise RuntimeError(
            "test device already has remote participants; use a dedicated "
            "empty device or pass --allow-existing"
        )
    if len(existing) > desired_remote_count:
        raise RuntimeError(
            f"device already has {len(existing)} remote participants, above "
            f"the requested capacity {desired_remote_count}"
        )

    run_id = secrets.token_hex(4)
    for index in range(desired_remote_count - len(existing)):
        participant_id = (
            f"{readiness_smoke.SMOKE_PARTICIPANT_PREFIX}capacity-"
            f"{run_id}-{index + 1}"
        )
        # Track the ID before the first persistent request so cleanup also
        # covers a device reset between approve/configure/activate.
        created.append(participant_id)
        readiness_smoke.provision_capacity_participant(client, participant_id)
        print(
            "device_group_capacity_smoke: provisioned "
            f"{len(existing) + len(created)}/{desired_remote_count} remote"
        )

    final = _participants(client)
    remote_count = int(final.body.get("registry", {}).get("remoteCount", -1))
    active_count = int(
        final.body.get("registry", {}).get("activeRemoteCount", -1)
    )
    if remote_count != desired_remote_count or active_count != desired_remote_count:
        raise RuntimeError(
            "capacity fixture did not reach the requested envelope: "
            f"remote={remote_count} active={active_count} "
            f"expected_remote={desired_remote_count}"
        )
    return created


def run_capacity_smoke(
    args: argparse.Namespace, created: list[str]
) -> dict:
    if not args.confirm_persistent_test_records:
        raise RuntimeError(
            "--confirm-persistent-test-records is required because the test "
            "temporarily writes participant records"
        )
    if args.remote_participants < 1 or (
        args.remote_participants > MAX_REMOTE_GROUP_PARTICIPANTS
    ):
        raise ValueError(
            "remote participants must be between 1 and "
            f"{MAX_REMOTE_GROUP_PARTICIPANTS}"
        )

    readiness_client = readiness_smoke.DeviceClient(
        args.device, timeout=args.timeout
    )
    provision_capacity_records(
        readiness_client,
        args.remote_participants,
        args.allow_existing,
        created,
    )
    limits = resource_smoke.ResourceLimits(
        min_internal_free_bytes=args.min_internal_free,
        min_internal_largest_block_bytes=args.min_largest_block,
        min_httpd_stack_free_bytes=args.min_httpd_stack,
        min_firmware_headroom_bytes=args.min_firmware_headroom,
        min_storage_headroom_bytes=args.min_storage_headroom,
        max_internal_free_drop_bytes=args.max_internal_drop,
        max_request_errors=args.max_request_errors,
    )
    report = resource_smoke.run_smoke(
        resource_smoke.HttpDeviceClient(args.device, timeout=args.timeout),
        limits,
        rounds=args.rounds,
        concurrency=args.concurrency,
        interval_seconds=args.interval,
        exercise=True,
        scope="lifecycle",
        duration_seconds=args.duration_seconds,
    )
    report["mode"] = "seven_participant_resource_capacity"
    report["capacityFixture"] = {
        "localParticipants": 1,
        "remoteParticipants": args.remote_participants,
        "totalParticipants": args.remote_participants + 1,
        "authenticatedObservationStreams": 0,
        "preexistingAllowed": args.allow_existing,
        "createdParticipantIds": created,
    }
    return report


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--remote-participants", type=int, default=6)
    parser.add_argument("--rounds", type=int, default=30)
    parser.add_argument("--concurrency", type=int, default=4)
    parser.add_argument("--interval", type=float, default=0.5)
    parser.add_argument("--duration-seconds", type=float)
    parser.add_argument("--allow-existing", action="store_true")
    parser.add_argument(
        "--confirm-persistent-test-records", action="store_true"
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument("--min-internal-free", type=int, default=48 * 1024)
    parser.add_argument("--min-largest-block", type=int, default=32 * 1024)
    parser.add_argument("--min-httpd-stack", type=int, default=2 * 1024)
    parser.add_argument(
        "--min-firmware-headroom", type=int, default=512 * 1024
    )
    parser.add_argument(
        "--min-storage-headroom", type=int, default=128 * 1024
    )
    parser.add_argument("--max-internal-drop", type=int, default=32 * 1024)
    parser.add_argument("--max-request-errors", type=int, default=0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    created: list[str] = []
    report: dict | None = None
    error: Exception | None = None
    cleanup_failures: list[str] = []
    try:
        report = run_capacity_smoke(args, created)
    except (RuntimeError, ValueError) as cause:
        error = cause
    finally:
        if created:
            client = readiness_smoke.DeviceClient(
                args.device, timeout=args.timeout
            )
            for participant_id in reversed(created):
                if not _cleanup_with_reconnect(client, participant_id):
                    cleanup_failures.append(participant_id)
            print(
                "device_group_capacity_smoke: cleanup "
                f"removed={len(created) - len(cleanup_failures)} "
                f"failed={len(cleanup_failures)}"
            )

    if report is not None:
        report["capacityFixture"]["cleanupFailures"] = cleanup_failures
        resource_smoke.print_report(report)
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(
                json.dumps(report, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
    if error is not None:
        print(f"device_group_capacity_smoke: error: {error}", file=sys.stderr)
        return 2
    if cleanup_failures:
        print(
            "device_group_capacity_smoke: cleanup failed for "
            + ", ".join(cleanup_failures),
            file=sys.stderr,
        )
        return 3
    return 0 if report is not None and report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

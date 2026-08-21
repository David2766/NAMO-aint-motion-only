#!/usr/bin/env python3
"""Exercise participant readiness, activation, and cleanup on a device."""

from __future__ import annotations

import argparse
import json
import secrets
import socket
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Protocol


@dataclass(frozen=True)
class Response:
    status: int
    body: dict[str, Any]


class Client(Protocol):
    def get(self, path: str) -> Response: ...
    def post_form(self, path: str, data: dict[str, Any]) -> Response: ...
    def send_datagram(self, port: int, payload: bytes) -> None: ...


class DeviceClient:
    def __init__(self, device: str, timeout: float = 5.0) -> None:
        self.base_url = device.rstrip("/")
        self.timeout = timeout

    def _request(self, path: str, method: str = "GET", data: bytes | None = None,
                 content_type: str | None = None) -> Response:
        headers = {"Accept": "application/json"}
        if content_type:
            headers["Content-Type"] = content_type
        request = urllib.request.Request(
            f"{self.base_url}{path}", data=data, headers=headers, method=method
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as result:
                status = result.status
                payload = result.read()
        except urllib.error.HTTPError as error:
            status = error.code
            payload = error.read()
        body = json.loads(payload.decode("utf-8")) if payload else {}
        return Response(status, body)

    def get(self, path: str) -> Response:
        return self._request(path)

    def post_form(self, path: str, data: dict[str, Any]) -> Response:
        encoded = urllib.parse.urlencode(data).encode("utf-8")
        return self._request(path, "POST", encoded,
                             "application/x-www-form-urlencoded")

    def send_datagram(self, port: int, payload: bytes) -> None:
        parsed = urllib.parse.urlparse(self.base_url)
        if parsed.scheme != "http" or not parsed.hostname:
            raise RuntimeError("transport smoke requires an http device URL")
        addresses = socket.getaddrinfo(
            parsed.hostname, port, type=socket.SOCK_DGRAM
        )
        _require(bool(addresses), "device hostname did not resolve for UDP")
        family, socktype, protocol, _, address = addresses[0]
        with socket.socket(family, socktype, protocol) as datagram:
            datagram.settimeout(self.timeout)
            datagram.sendto(payload, address)

def _require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def _participant(response: Response, participant_id: str) -> dict[str, Any] | None:
    for participant in response.body.get("participants", []):
        if participant.get("participantId") == participant_id:
            return participant
    return None


def _error_code(response: Response) -> str | None:
    error_info = response.body.get("errorInfo")
    if isinstance(error_info, dict) and isinstance(error_info.get("code"), str):
        return error_info["code"]
    error = response.body.get("error")
    return error if isinstance(error, str) else None


def _require_status(response: Response, expected: int, message: str) -> None:
    code = _error_code(response)
    suffix = f"status={response.status}"
    if code:
        suffix += f" code={code}"
    _require(response.status == expected, f"{message}: {suffix}")


def _post_participant(client: Client, **data: Any) -> Response:
    return client.post_form("/api/platform/participants", data)


def _configure_spatial(client: Client, participant_id: str, pose_version: int,
                       calibration_version: int,
                       include_transform: bool = False) -> Response:
    fields: dict[str, Any] = {
        "action": "configure",
        "participantId": participant_id,
        "pointTracks": "true",
        "velocity": "true",
        "positionUncertainty": "true",
        "range": "true",
        "presence": "true",
        "motion": "true",
        "zonePresence": "false",
        "bridge": "false",
        "maxTargets": 3,
        "floorplanId": "smoke-floor",
        "spaceId": "smoke-room",
        "groupId": "smoke-group",
        "geometryVersion": 1,
        "localPoseVersion": 1,
        "poseVersion": pose_version,
        "calibrationVersion": calibration_version,
    }
    if include_transform:
        fields.update({
            "originXMillimeters": 1234,
            "originYMillimeters": 5678,
            "rotationCentiDegrees": 1234,
            "residualMillimeters": 120,
            "calibrationSamples": 2,
        })
    return _post_participant(client, **fields)


def _configure_presence_only(client: Client, participant_id: str) -> Response:
    return _post_participant(
        client,
        action="configure",
        participantId=participant_id,
        pointTracks="false",
        velocity="false",
        positionUncertainty="false",
        range="true",
        presence="true",
        motion="true",
        zonePresence="false",
        bridge="false",
        maxTargets=0,
        floorplanId="smoke-floor",
        spaceId="smoke-room",
        groupId="smoke-group",
        geometryVersion=0,
        localPoseVersion=0,
        poseVersion=0,
        calibrationVersion=0,
    )


SMOKE_PARTICIPANT_PREFIX = "satellite-smoke-"
TRANSPORT_SMOKE_PARTICIPANT_ID = "satellite-smoke-transport"
TRANSPORT_SMOKE_PORT = 49190
TRANSPORT_SMOKE_KEY = bytes(range(32))


def _load_transport_codec():
    simulator_dir = Path(__file__).resolve().parent / "platform-sim"
    path = str(simulator_dir)
    if path not in sys.path:
        sys.path.insert(0, path)
    from codec import encode_authenticated_observation
    return encode_authenticated_observation


def _transport_observation(site_id: str, boot_id: str,
                           sequence: int) -> dict[str, Any]:
    return {
        "protocolVersion": 1,
        "siteId": site_id,
        "participantId": TRANSPORT_SMOKE_PARTICIPANT_ID,
        "participantType": "satellite",
        "bootId": boot_id,
        "groupId": "smoke-group",
        "sequence": sequence,
        "observedAtMs": int(time.monotonic() * 1000) & 0xFFFFFFFF,
        "ttlMs": 1500,
        "frame": {
            "id": "sensor-local",
            "poseVersion": 1,
            "calibrationVersion": 1,
        },
        "tracks": [{
            "localTrackId": 1,
            "xMm": 350.0,
            "yMm": 1200.0,
            "vxMmS": 0.0,
            "vyMmS": 0.0,
            "qualityScore": 90,
            "sampleAgeMs": 0,
        }],
        "evidence": {
            "presence": True,
            "motion": False,
            "rangeMm": 1250.0,
            "zonePresence": [],
        },
        "health": {"state": "healthy", "sampleAgeMs": 0},
    }


def _shadow_transport(response: Response) -> dict[str, Any]:
    value = response.body.get("shadowTransport")
    _require(isinstance(value, dict),
             "participants response is missing shadowTransport diagnostics")
    return value


def _wait_for_transport(
    client: Client, predicate, message: str, timeout: float = 3.0
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    while True:
        diagnostics = _shadow_transport(
            client.get("/api/platform/participants")
        )
        if predicate(diagnostics):
            return diagnostics
        if time.monotonic() >= deadline:
            raise RuntimeError(message)
        time.sleep(0.05)


def _run_transport_smoke(client: Client, site_id: str) -> None:
    encode = _load_transport_codec()
    boot_id = f"transport-smoke-{secrets.token_hex(8)}"
    baseline = _shadow_transport(client.get("/api/platform/participants"))
    _require(baseline.get("smokeMode") is True,
             "firmware was not built with --transport-smoke")
    _require(baseline.get("socketReady") is True,
             "shadow transport UDP socket is not ready")
    _require(baseline.get("port") == TRANSPORT_SMOKE_PORT,
             "shadow transport is listening on an unexpected port")

    counters = {
        name: int(baseline.get(name, 0))
        for name in ("received", "authenticated", "admitted", "rejected",
                     "rateLimited")
    }

    def packet(sequence: int) -> bytes:
        return encode(
            _transport_observation(site_id, boot_id, sequence),
            TRANSPORT_SMOKE_KEY,
        )

    # The device-side limiter survives script invocations within the same boot.
    time.sleep(1.05)
    client.send_datagram(TRANSPORT_SMOKE_PORT, packet(1))
    client.send_datagram(TRANSPORT_SMOKE_PORT, packet(2))
    client.send_datagram(TRANSPORT_SMOKE_PORT, packet(3))

    tampered = bytearray(packet(4))
    tampered[-1] ^= 0x01
    client.send_datagram(TRANSPORT_SMOKE_PORT, bytes(tampered))
    _wait_for_transport(
        client,
        lambda item: int(item.get("received", 0)) >= counters["received"] + 4,
        "device did not consume the initial transport packets",
    )

    time.sleep(1.05)
    client.send_datagram(TRANSPORT_SMOKE_PORT, packet(2))
    final = _wait_for_transport(
        client,
        lambda item: int(item.get("received", 0)) >= counters["received"] + 5,
        "device did not consume the duplicate transport packet",
    )

    _require(int(final.get("authenticated", 0)) >= counters["authenticated"] + 4,
             "valid transport packets were not authenticated")
    _require(int(final.get("admitted", 0)) >= counters["admitted"] + 2,
             "valid transport packets were not admitted")
    _require(int(final.get("rejected", 0)) >= counters["rejected"] + 3,
             "invalid transport packets were not rejected")
    _require(int(final.get("rateLimited", 0)) >= counters["rateLimited"] + 1,
             "per-participant transport rate limit was not enforced")
    _require(final.get("lastResult") == "admission_rejected" and
             final.get("lastAdmission") == "duplicate_or_reordered",
             "duplicate observation was not rejected by admission control")
    _require(int(final.get("maxProcessUs", 0)) > 0,
             "transport processing time was not measured")


def _smoke_participant_ids(response: Response) -> list[str]:
    ids: list[str] = []
    for participant in response.body.get("participants", []):
        participant_id = participant.get("participantId")
        if isinstance(participant_id, str) and participant_id.startswith(
                SMOKE_PARTICIPANT_PREFIX):
            ids.append(participant_id)
    return ids


def _cleanup(client: Client, participant_id: str, attempts: int = 3) -> bool:
    try:
        _post_participant(client, action="close")
    except (OSError, urllib.error.URLError, TimeoutError):
        return False

    for attempt in range(attempts):
        try:
            current = client.get("/api/platform/participants")
            participant = _participant(current, participant_id)
            if current.status == 200 and participant is None:
                return True

            if participant is not None and participant.get("membership") == "active":
                quarantined = _post_participant(
                    client, action="quarantine", participantId=participant_id
                )
                if quarantined.status != 200:
                    if attempt + 1 < attempts:
                        time.sleep(0.25)
                    continue

            forgotten = _post_participant(
                client, action="forget", participantId=participant_id
            )
            if forgotten.status != 200 and attempt + 1 < attempts:
                time.sleep(0.25)
        except (OSError, urllib.error.URLError, TimeoutError):
            return False

    try:
        final = client.get("/api/platform/participants")
    except (OSError, urllib.error.URLError, TimeoutError):
        return False
    return final.status == 200 and _participant(final, participant_id) is None


def _cleanup_stale_smoke_participants(client: Client, response: Response) -> None:
    for stale_id in _smoke_participant_ids(response):
        _require(_cleanup(client, stale_id),
                 f"failed to remove stale smoke participant: {stale_id}")


def provision_capacity_participant(client: Client, participant_id: str) -> None:
    """Persist one full spatial Satellite until the caller cleans it up."""
    _require(participant_id.startswith(SMOKE_PARTICIPANT_PREFIX),
             "capacity participant id must use the smoke prefix")
    try:
        _require_status(
            _post_participant(client, action="open", durationSeconds=120),
            200,
            "failed to open pairing window",
        )
        _require_status(
            _post_participant(
                client,
                action="stage",
                participantId=participant_id,
                participantType="satellite",
                protocolVersion=1,
                product="participant-capacity-smoke",
            ),
            200,
            "failed to stage capacity participant",
        )
        _require_status(_post_participant(client, action="approve"), 200,
                        "failed to approve capacity participant")
        _require_status(
            _configure_spatial(
                client,
                participant_id,
                pose_version=1,
                calibration_version=1,
                include_transform=True,
            ),
            200,
            "failed to configure capacity participant",
        )
        _require_status(
            _post_participant(
                client, action="activate", participantId=participant_id
            ),
            200,
            "failed to activate capacity participant",
        )
        current = client.get("/api/platform/participants")
        participant = _participant(current, participant_id)
        _require(
            current.status == 200 and participant is not None and
            participant.get("membership") == "active" and
            participant.get("readiness", {}).get("ready") is True,
            "capacity participant did not become active and ready",
        )
    finally:
        try:
            _post_participant(client, action="close")
        except (OSError, urllib.error.URLError, TimeoutError):
            pass


def run_smoke(client: Client, participant_id: str, transport: bool = False) -> None:
    initial = client.get("/api/platform/participants")
    _require(initial.status == 200, "participants API is unavailable")
    _require(initial.body.get("registry", {}).get("ready") is True,
             "participant registry is not ready")
    _require(initial.body.get("readiness", {}).get("ready") is True,
             "participant readiness storage is not ready")
    _require(initial.body.get("credentials", {}).get("ready") is True,
             "participant credential storage is not ready")
    _cleanup_stale_smoke_participants(client, initial)
    initial = client.get("/api/platform/participants")
    _require(_participant(initial, participant_id) is None,
             f"participant id already exists: {participant_id}")

    if transport:
        status = client.get("/api/system/status")
        _require_status(status, 200, "system status is unavailable")
        site_id = status.body.get("platform", {}).get("siteId")
        _require(isinstance(site_id, str) and site_id,
                 "system status is missing platform.siteId")
        _run_transport_smoke(client, site_id)
        return

    cleanup_complete = False
    try:
        _require_status(_post_participant(client, action="open", durationSeconds=120),
                        200, "failed to open pairing window")
        _require_status(_post_participant(
            client,
            action="stage",
            participantId=participant_id,
            participantType="satellite",
            protocolVersion=1,
            product="participant-readiness-smoke",
        ), 200, "failed to stage participant")
        _require_status(_post_participant(client, action="approve"), 200,
                        "failed to approve participant")

        approved = client.get("/api/platform/participants")
        record = _participant(approved, participant_id)
        _require(record is not None and record.get("membership") == "quarantined",
                 "approval must leave the participant quarantined")

        blocked = _post_participant(client, action="activate",
                                    participantId=participant_id)
        _require(blocked.status == 409 and _error_code(blocked) == "not_ready",
                 "activation without readiness must fail with not_ready")

        _require_status(_configure_spatial(client, participant_id, 0, 0), 200,
                        "failed to save incomplete spatial readiness")
        blocked = _post_participant(client, action="activate",
                                    participantId=participant_id)
        _require(blocked.status == 409 and _error_code(blocked) == "not_ready",
                 "spatial activation without pose/calibration must be blocked")

        _require_status(_configure_presence_only(client, participant_id), 200,
                        "failed to save presence-only readiness")
        _require_status(_post_participant(client, action="activate",
                                         participantId=participant_id),
                        200, "failed to activate presence-only participant")
        active = client.get("/api/platform/participants")
        record = _participant(active, participant_id)
        _require(record is not None and record.get("membership") == "active" and
                 record.get("readiness", {}).get("ready") is True,
                 "active participant response is missing readiness")

    finally:
        cleanup_complete = _cleanup(client, participant_id)

    _require(cleanup_complete, "smoke participant cleanup did not persist")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True,
                        help="Device origin, for example http://192.168.29.104")
    parser.add_argument("--participant-id",
                        default=None)
    parser.add_argument(
        "--transport",
        action="store_true",
        help="also exercise the development-only authenticated UDP receiver",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    participant_id = args.participant_id or (
        TRANSPORT_SMOKE_PARTICIPANT_ID if args.transport
        else f"satellite-smoke-{int(time.time())}"
    )
    if args.transport and participant_id != TRANSPORT_SMOKE_PARTICIPANT_ID:
        raise RuntimeError(
            f"--transport requires participant id {TRANSPORT_SMOKE_PARTICIPANT_ID}"
        )
    run_smoke(DeviceClient(args.device), participant_id,
              transport=args.transport)
    print("participant_readiness_smoke: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

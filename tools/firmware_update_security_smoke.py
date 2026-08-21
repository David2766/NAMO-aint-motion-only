#!/usr/bin/env python3
"""Non-destructive signed OTA preflight checks against a physical device."""

from __future__ import annotations

import argparse
import json
import struct
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path


HEADER_SIZE = 320
MAX_IMAGE_SIZE = 0x300000
MAGIC = b"NAMOOTA1"


class SmokeFailure(RuntimeError):
    pass


def request_json(
    origin: str,
    path: str,
    *,
    values: dict[str, str] | None = None,
    timeout: float,
) -> tuple[int, dict[str, object]]:
    data = None
    method = "GET"
    headers: dict[str, str] = {"Cache-Control": "no-store"}
    if values is not None:
        data = urllib.parse.urlencode(values).encode("ascii")
        method = "POST"
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    request = urllib.request.Request(
        f"{origin.rstrip('/')}{path}", data=data, headers=headers, method=method
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            status = response.status
            body = response.read()
    except urllib.error.HTTPError as error:
        status = error.code
        body = error.read()
    try:
        parsed = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise SmokeFailure(f"{path} returned invalid JSON with HTTP {status}") from error
    if not isinstance(parsed, dict):
        raise SmokeFailure(f"{path} returned a non-object JSON response")
    return status, parsed


def request_multipart_probe(
    origin: str, path: str, *, timeout: float
) -> tuple[int, dict[str, object]]:
    boundary = "namo-security-route-probe"
    body = (
        f"--{boundary}\r\n"
        'Content-Disposition: form-data; name="file"; filename="probe.bin"\r\n'
        "Content-Type: application/octet-stream\r\n\r\n"
    ).encode("ascii") + b"\x00" + f"\r\n--{boundary}--\r\n".encode("ascii")
    request = urllib.request.Request(
        f"{origin.rstrip('/')}{path}",
        data=body,
        headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            status = response.status
            payload = response.read()
    except urllib.error.HTTPError as error:
        status = error.code
        payload = error.read()
    try:
        parsed = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise SmokeFailure(
            f"multipart route probe returned invalid JSON with HTTP {status}"
        ) from error
    if not isinstance(parsed, dict):
        raise SmokeFailure("multipart route probe returned non-object JSON")
    return status, parsed


def read_package(path: Path) -> bytes:
    package = path.read_bytes()
    if len(package) < HEADER_SIZE or package[:8] != MAGIC:
        raise SmokeFailure("package is not a NAMO signed OTA file")
    image_size = struct.unpack_from("<I", package, 24)[0]
    if image_size < 1 or image_size > MAX_IMAGE_SIZE:
        raise SmokeFailure("package image size is outside the firmware contract")
    if len(package) != HEADER_SIZE + image_size:
        raise SmokeFailure("package length does not match its signed image size")
    return package


def expect_rejected(
    origin: str, header: bytes, *, timeout: float, label: str
) -> None:
    status, response = request_json(
        origin,
        "/api/firmware/update/start",
        values={"header": header.hex()},
        timeout=timeout,
    )
    if status < 400 or response.get("ok") is True:
        raise SmokeFailure(f"{label} was accepted with HTTP {status}")


def run(origin: str, package: bytes, timeout: float) -> None:
    status, trust = request_json(
        origin, "/api/firmware/trust", timeout=timeout
    )
    data = trust.get("data")
    if (
        status != 200
        or trust.get("ok") is not True
        or not isinstance(data, dict)
        or data.get("mode") != "signed"
        or data.get("ready") is not True
    ):
        raise SmokeFailure("device signed OTA trust is not ready")

    header = package[:HEADER_SIZE]
    status, started = request_json(
        origin,
        "/api/firmware/update/start",
        values={"header": header.hex()},
        timeout=timeout,
    )
    started_data = started.get("data")
    session = started_data.get("session") if isinstance(started_data, dict) else None
    if status != 200 or started.get("ok") is not True or not isinstance(session, int) or session < 1:
        raise SmokeFailure("valid signed preflight was rejected")

    wrong_session = 1 if session == 0xFFFFFFFF else session + 1
    wrong_status, _ = request_json(
        origin,
        f"/api/firmware/update/cancel?session={wrong_session}",
        values={},
        timeout=timeout,
    )
    if wrong_status != 409:
        raise SmokeFailure("wrong cancel token did not return HTTP 409")
    _, active = request_json(origin, "/api/firmware/trust", timeout=timeout)
    active_data = active.get("data")
    if not isinstance(active_data, dict) or active_data.get("sessionActive") is not True:
        raise SmokeFailure("wrong cancel token terminated the valid session")

    probe_status, _ = request_multipart_probe(
        origin,
        f"/api/firmware/update/start?session={session}",
        timeout=timeout,
    )
    if probe_status < 400:
        raise SmokeFailure("multipart route-confusion probe was unexpectedly accepted")
    _, active = request_json(origin, "/api/firmware/trust", timeout=timeout)
    active_data = active.get("data")
    if not isinstance(active_data, dict) or active_data.get("sessionActive") is not True:
        raise SmokeFailure("a non-upload multipart route changed the valid OTA session")

    cancel_status, cancelled = request_json(
        origin,
        f"/api/firmware/update/cancel?session={session}",
        values={},
        timeout=timeout,
    )
    if cancel_status != 200 or cancelled.get("ok") is not True:
        raise SmokeFailure("matching cancel token did not close the session")

    expect_rejected(origin, header[:-1], timeout=timeout, label="truncated header")
    expect_rejected(origin, header + b"\x00", timeout=timeout, label="oversized header")
    for index in range(HEADER_SIZE):
        changed = bytearray(header)
        changed[index] ^= 1
        expect_rejected(
            origin,
            bytes(changed),
            timeout=timeout,
            label=f"header mutation at byte {index}",
        )

    _, final_trust = request_json(origin, "/api/firmware/trust", timeout=timeout)
    final_data = final_trust.get("data")
    if not isinstance(final_data, dict) or final_data.get("sessionActive") is not False:
        raise SmokeFailure("a rejected preflight left an OTA session active")

    print(
        "firmware_update_security_smoke: ok "
        f"mutations={HEADER_SIZE} trustRevision={data.get('trustRevision')} "
        f"securityEpoch={data.get('securityEpoch')}"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", required=True, help="Device origin, for example http://192.168.29.218")
    parser.add_argument("--package", required=True, type=Path, help="Official .namo-ota package")
    parser.add_argument("--timeout-seconds", type=float, default=10.0)
    args = parser.parse_args()
    try:
        run(args.device, read_package(args.package), args.timeout_seconds)
    except (OSError, SmokeFailure, urllib.error.URLError) as error:
        print(f"firmware_update_security_smoke: failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Low-rate, read-only multi-device capture for office noise analysis."""

from __future__ import annotations

import argparse
from concurrent.futures import Future, ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import re
import time
from typing import Any, Callable
import urllib.error
import urllib.parse
import urllib.request


SCHEMA_VERSION = 1
DEFAULT_STATE_INTERVAL_SECONDS = 1.0
DEFAULT_GROUP_INTERVAL_SECONDS = 10.0
DEFAULT_SYSTEM_INTERVAL_SECONDS = 60.0
DEFAULT_ROTATE_SECONDS = 3600.0
MAX_DEVICES = 7


@dataclass(frozen=True)
class DeviceSpec:
    alias: str
    base_url: str


class HttpJsonClient:
    def __init__(self, base_url: str, timeout_seconds: float) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout_seconds = timeout_seconds

    def get_json(self, path: str) -> tuple[dict[str, Any], float]:
        request = urllib.request.Request(
            f"{self.base_url}{path}",
            headers={
                "Accept": "application/json",
                "Cache-Control": "no-cache",
            },
            method="GET",
        )
        started = time.monotonic()
        try:
            with urllib.request.urlopen(
                request, timeout=self.timeout_seconds
            ) as response:
                status = response.status
                payload = response.read()
        except urllib.error.HTTPError as error:
            detail = error.read().decode("utf-8", errors="replace")
            raise RuntimeError(
                f"GET {path} failed: status={error.code} "
                f"body={detail[:160]}"
            ) from error
        except urllib.error.URLError as error:
            raise RuntimeError(
                f"GET {path} failed: {error.reason}"
            ) from error
        except (TimeoutError, OSError) as error:
            raise RuntimeError(f"GET {path} failed: {error}") from error
        latency_ms = (time.monotonic() - started) * 1000.0
        if status != 200:
            raise RuntimeError(f"GET {path} failed: status={status}")
        try:
            body = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise RuntimeError(
                f"GET {path} returned invalid JSON"
            ) from error
        if not isinstance(body, dict):
            raise RuntimeError(f"GET {path} returned a non-object JSON payload")
        return body, latency_ms


def _safe_alias(value: str) -> str:
    alias = re.sub(r"[^A-Za-z0-9_-]+", "-", value).strip("-")
    if not alias:
        raise ValueError("device alias is empty")
    return alias[:32]


def parse_device_spec(value: str) -> DeviceSpec:
    raw = value.strip()
    if not raw:
        raise ValueError("device must not be empty")
    if "=" in raw and not raw.startswith(("http://", "https://")):
        alias_text, url_text = raw.split("=", 1)
        alias = _safe_alias(alias_text)
        base_url = url_text.strip().rstrip("/")
    else:
        base_url = raw.rstrip("/")
        parsed = urllib.parse.urlparse(base_url)
        host = parsed.hostname or ""
        if re.fullmatch(r"\d+\.\d+\.\d+\.\d+", host):
            alias = _safe_alias(host.rsplit(".", 1)[-1])
        else:
            alias = _safe_alias(host)
    parsed = urllib.parse.urlparse(base_url)
    if parsed.scheme not in ("http", "https") or not parsed.hostname:
        raise ValueError(f"invalid device URL: {base_url}")
    if parsed.path not in ("", "/") or parsed.query or parsed.fragment:
        raise ValueError("device URL must not include a path, query, or fragment")
    return DeviceSpec(alias=alias, base_url=base_url)


def parse_devices(values: list[str]) -> list[DeviceSpec]:
    devices = [parse_device_spec(value) for value in values]
    if not devices or len(devices) > MAX_DEVICES:
        raise ValueError(f"device count must be between 1 and {MAX_DEVICES}")
    aliases = [device.alias for device in devices]
    urls = [device.base_url for device in devices]
    if len(set(aliases)) != len(aliases):
        raise ValueError("device aliases must be unique")
    if len(set(urls)) != len(urls):
        raise ValueError("device URLs must be unique")
    return devices


def _object(value: Any) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def _array(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def _number(value: Any) -> int | float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    if isinstance(value, float) and not math.isfinite(value):
        return None
    return value


def _compact_targets(value: Any) -> list[dict[str, Any]]:
    targets: list[dict[str, Any]] = []
    for raw in _array(value):
        target = _object(raw)
        if target.get("active") is not True:
            continue
        compact: dict[str, Any] = {
            "x": _number(target.get("x")),
            "y": _number(target.get("y")),
        }
        for key in ("id", "name"):
            if isinstance(target.get(key), str):
                compact[key] = target[key]
        if isinstance(target.get("fused"), bool):
            compact["fused"] = target["fused"]
        if _number(target.get("sourceCount")) is not None:
            compact["sourceCount"] = target["sourceCount"]
        targets.append(compact)
    return targets


def compact_state(payload: dict[str, Any]) -> dict[str, Any]:
    fusion = _object(payload.get("fusion"))
    local = _object(payload.get("local"))
    automation = _object(payload.get("automation"))
    debug = _object(payload.get("debug"))
    static_radar = _object(debug.get("staticRadar"))
    assist = _object(static_radar.get("assist"))
    room = _object(debug.get("room"))
    tracker = _object(debug.get("tracker"))
    return {
        "connected": payload.get("connected"),
        "presence": payload.get("presence"),
        "motion": payload.get("motion"),
        "targetCount": _number(payload.get("targetCount")),
        "movingTargetCount": _number(payload.get("movingTargetCount")),
        "stillTargetCount": _number(payload.get("stillTargetCount")),
        "filterBlocked": payload.get("filterBlocked"),
        "targets": _compact_targets(payload.get("targets")),
        "fusion": {
            "authoritative": fusion.get("authoritative"),
            "state": fusion.get("state"),
            "source": fusion.get("source"),
            "groupTerm": _number(fusion.get("groupTerm")),
            "sequence": _number(fusion.get("sequence")),
            "sourceSequence": _number(fusion.get("sourceSequence")),
        },
        "local": {
            "presence": local.get("presence"),
            "motion": local.get("motion"),
            "targetCount": _number(local.get("targetCount")),
            "targets": _compact_targets(local.get("targets")),
        },
        "automation": {
            "available": automation.get("available"),
            "health": automation.get("health"),
            "continuityHeld": automation.get("continuityHeld"),
            "holdAgeMs": _number(automation.get("holdAgeMs")),
            "holdRemainingMs": _number(
                automation.get("holdRemainingMs")
            ),
            "continuitySourceLeaderNodeId": automation.get(
                "continuitySourceLeaderNodeId"
            ),
            "continuitySourceGroupTerm": _number(
                automation.get("continuitySourceGroupTerm")
            ),
            "leaderNodeId": automation.get("leaderNodeId"),
            "groupTerm": _number(automation.get("groupTerm")),
            "ageMs": _number(automation.get("ageMs")),
            "presence": automation.get("presence"),
            "motion": automation.get("motion"),
            "targetCount": _number(automation.get("targetCount")),
        },
        "debug": {
            "presenceReason": debug.get("presenceReason"),
            "presenceOffReason": debug.get("presenceOffReason"),
            "motionReason": debug.get("motionReason"),
            "tracker": {
                "presence": tracker.get("presence"),
                "motion": tracker.get("motion"),
                "state": tracker.get("state"),
                "reason": tracker.get("reason"),
            },
            "staticRadar": {
                "available": static_radar.get("available"),
                "presence": static_radar.get("presence"),
                "moving": static_radar.get("moving"),
                "still": static_radar.get("still"),
                "reason": static_radar.get("reason"),
                "assistActive": assist.get("active"),
                "exitVeto": assist.get("exitVeto"),
            },
            "room": {
                "configured": room.get("configured"),
                "insideTargetCount": _number(room.get("insideTargetCount")),
                "outsideTargetCount": _number(room.get("outsideTargetCount")),
            },
        },
    }


def compact_groups(payload: dict[str, Any]) -> dict[str, Any]:
    raw_site = _object(payload.get("site"))
    groups: list[dict[str, Any]] = []
    for raw in _array(payload.get("groups")):
        group = _object(raw)
        if group.get("configured") is not True:
            continue
        groups.append({
            "groupId": group.get("groupId"),
            "leaderNodeId": group.get("leaderNodeId"),
            "localLeader": group.get("localLeader"),
            "term": _number(group.get("term")),
            "participantsOnline": _number(group.get("participantsOnline")),
            "participantsExpected": _number(
                group.get("participantsExpected")
            ),
            "quorumRequired": _number(group.get("quorumRequired")),
            "quorumAvailable": group.get("quorumAvailable"),
            "canonicalAssignment": group.get("canonicalAssignment"),
            "credentialsComplete": group.get("credentialsComplete"),
            "health": group.get("health"),
        })
    shadow = _object(payload.get("shadowFusion"))
    shadow_diagnostics = _object(shadow.get("diagnostics"))
    production = _object(payload.get("productionFusion"))
    return {
        "site": {
            "leaderNodeId": raw_site.get("leaderNodeId"),
            "localLeader": raw_site.get("localLeader"),
            "term": _number(raw_site.get("term")),
            "participantsOnline": _number(
                raw_site.get("participantsOnline")
            ),
            "participantsExpected": _number(
                raw_site.get("participantsExpected")
            ),
            "quorumRequired": _number(raw_site.get("quorumRequired")),
            "quorumAvailable": raw_site.get("quorumAvailable"),
            "health": raw_site.get("health"),
        },
        "groups": groups,
        "shadowFusion": {
            "state": shadow.get("state"),
            "trackCount": _number(shadow.get("trackCount")),
            "maxCycleUs": _number(shadow.get("maxCycleUs")),
            "validInputs": _number(shadow_diagnostics.get("validInputs")),
            "candidatePairs": _number(
                shadow_diagnostics.get("candidatePairs")
            ),
            "pairedMeasurements": _number(
                shadow_diagnostics.get("pairedMeasurements")
            ),
            "ambiguousPairs": _number(
                shadow_diagnostics.get("ambiguousPairs")
            ),
            "droppedMeasurements": _number(
                shadow_diagnostics.get("droppedMeasurements")
            ),
            "tentativeTracks": _number(
                shadow_diagnostics.get("tentativeTracks")
            ),
            "confirmedTracks": _number(
                shadow_diagnostics.get("confirmedTracks")
            ),
            "coastingTracks": _number(
                shadow_diagnostics.get("coastingTracks")
            ),
        },
        "productionFusion": {
            "authoritative": production.get("authoritative"),
            "state": production.get("state"),
            "sourceSequence": _number(production.get("sourceSequence")),
            "targetCount": _number(production.get("targetCount")),
        },
    }


def compact_configuration(payload: dict[str, Any]) -> dict[str, Any]:
    return {
        "siteId": payload.get("siteId"),
        "configurationOwnerNodeId": payload.get(
            "configurationOwnerNodeId"
        ),
        "localOwner": payload.get("localOwner"),
        "authorityEpoch": _number(payload.get("authorityEpoch")),
        "generation": _number(payload.get("generation")),
        "rosterRevision": _number(payload.get("rosterRevision")),
        "configurationConsistent": payload.get("configurationConsistent"),
        "sharedWritesAllowed": payload.get("sharedWritesAllowed"),
        "memberCount": _number(payload.get("memberCount")),
        "activeMemberCount": _number(payload.get("activeMemberCount")),
    }


def compact_system(payload: dict[str, Any]) -> dict[str, Any]:
    firmware = _object(payload.get("firmware"))
    platform = _object(payload.get("platform"))
    memory = _object(payload.get("memory"))
    wifi = _object(payload.get("wifi"))
    return {
        "firmware": {
            "version": firmware.get("version"),
            "buildTime": firmware.get("buildTime"),
            "uptimeSeconds": _number(firmware.get("uptimeSeconds")),
        },
        "platform": {
            "nodeId": platform.get("nodeId"),
            "siteId": platform.get("siteId"),
            "activeRemoteParticipantCount": _number(
                platform.get("activeRemoteParticipantCount")
            ),
        },
        "memory": {
            "internalFreeBytes": _number(memory.get("internalFreeBytes")),
            "internalMinFreeBytes": _number(
                memory.get("internalMinFreeBytes")
            ),
            "internalLargestFreeBlockBytes": _number(
                memory.get("internalLargestFreeBlockBytes")
            ),
            "httpdStackMinFreeBytes": _number(
                memory.get("httpdStackMinFreeBytes")
            ),
        },
        "wifi": {
            "connected": wifi.get("connected"),
            "rssi": _number(wifi.get("rssi")),
        },
    }


class RotatingJsonlWriter:
    def __init__(
        self, output_dir: Path, prefix: str, rotate_seconds: float
    ) -> None:
        self.output_dir = output_dir
        self.prefix = _safe_alias(prefix)
        self.rotate_seconds = rotate_seconds
        self.session_stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        self.part = 0
        self.next_rotation = 0.0
        self.file = None
        self.paths: list[Path] = []

    def write(
        self,
        record: dict[str, Any],
        elapsed_seconds: float,
        *,
        allow_rotate: bool = True,
    ) -> None:
        if self.file is None or (
            allow_rotate and elapsed_seconds >= self.next_rotation
        ):
            self._rotate(elapsed_seconds)
        record["filePart"] = self.part
        assert self.file is not None
        self.file.write(
            json.dumps(record, ensure_ascii=False, separators=(",", ":"))
            + "\n"
        )
        self.file.flush()

    def close(self) -> None:
        if self.file is not None:
            self.file.close()
            self.file = None

    def _rotate(self, elapsed_seconds: float) -> None:
        self.close()
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.part += 1
        path = self.output_dir / (
            f"{self.prefix}-{self.session_stamp}-part{self.part:04d}.jsonl"
        )
        self.file = path.open("x", encoding="utf-8", newline="\n")
        self.paths.append(path)
        rotation = math.floor(elapsed_seconds / self.rotate_seconds) + 1
        self.next_rotation = rotation * self.rotate_seconds


def _captured_at() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def collect_endpoint(
    executor: ThreadPoolExecutor,
    devices: list[DeviceSpec],
    clients: dict[str, HttpJsonClient],
    path: str,
    compact: Callable[[dict[str, Any]], dict[str, Any]],
) -> tuple[dict[str, Any], int]:
    futures: dict[Future[tuple[dict[str, Any], float]], DeviceSpec] = {
        executor.submit(clients[device.alias].get_json, path): device
        for device in devices
    }
    results: dict[str, Any] = {}
    error_count = 0
    for future in as_completed(futures):
        device = futures[future]
        try:
            payload, latency_ms = future.result()
            results[device.alias] = {
                "ok": True,
                "latencyMs": round(latency_ms, 3),
                "data": compact(payload),
            }
        except Exception as error:  # Continue capture after device failures.
            error_count += 1
            results[device.alias] = {
                "ok": False,
                "error": str(error)[:240],
            }
    return {
        device.alias: results[device.alias]
        for device in devices
    }, error_count


def _advance_deadline(current: float, interval: float, now: float) -> float:
    while current <= now:
        current += interval
    return current


def run_capture(
    devices: list[DeviceSpec],
    *,
    duration_seconds: float,
    state_interval_seconds: float,
    group_interval_seconds: float,
    system_interval_seconds: float,
    timeout_seconds: float,
    writer: RotatingJsonlWriter,
) -> dict[str, Any]:
    clients = {
        device.alias: HttpJsonClient(device.base_url, timeout_seconds)
        for device in devices
    }
    started = time.monotonic()
    next_state = 0.0
    next_groups = 0.0
    next_system = 0.0
    state_samples = 0
    group_samples = 0
    configuration_samples = 0
    system_samples = 0
    request_errors = 0
    interrupted = False
    writer.write({
        "schemaVersion": SCHEMA_VERSION,
        "type": "session",
        "capturedAt": _captured_at(),
        "elapsedSeconds": 0.0,
        "devices": {
            device.alias: {"baseUrl": device.base_url}
            for device in devices
        },
        "intervals": {
            "stateSeconds": state_interval_seconds,
            "groupsSeconds": group_interval_seconds,
            "configurationSeconds": group_interval_seconds,
            "systemSeconds": system_interval_seconds,
        },
    }, 0.0)

    try:
        with ThreadPoolExecutor(max_workers=len(devices)) as executor:
            while True:
                elapsed = time.monotonic() - started
                if elapsed >= duration_seconds:
                    break
                if elapsed >= next_state:
                    result, errors = collect_endpoint(
                        executor, devices, clients, "/api/state", compact_state
                    )
                    request_errors += errors
                    state_samples += 1
                    writer.write({
                        "schemaVersion": SCHEMA_VERSION,
                        "type": "state",
                        "capturedAt": _captured_at(),
                        "elapsedSeconds": round(elapsed, 3),
                        "devices": result,
                    }, elapsed)
                    next_state = _advance_deadline(
                        next_state, state_interval_seconds, elapsed
                    )
                if elapsed >= next_groups:
                    result, errors = collect_endpoint(
                        executor,
                        devices,
                        clients,
                        "/api/platform/groups",
                        compact_groups,
                    )
                    request_errors += errors
                    group_samples += 1
                    writer.write({
                        "schemaVersion": SCHEMA_VERSION,
                        "type": "groups",
                        "capturedAt": _captured_at(),
                        "elapsedSeconds": round(elapsed, 3),
                        "devices": result,
                    }, elapsed)
                    next_groups = _advance_deadline(
                        next_groups, group_interval_seconds, elapsed
                    )
                    result, errors = collect_endpoint(
                        executor,
                        devices,
                        clients,
                        "/api/platform/site-configuration",
                        compact_configuration,
                    )
                    request_errors += errors
                    configuration_samples += 1
                    writer.write({
                        "schemaVersion": SCHEMA_VERSION,
                        "type": "configuration",
                        "capturedAt": _captured_at(),
                        "elapsedSeconds": round(elapsed, 3),
                        "devices": result,
                    }, elapsed)
                if elapsed >= next_system:
                    result, errors = collect_endpoint(
                        executor,
                        devices,
                        clients,
                        "/api/system/status",
                        compact_system,
                    )
                    request_errors += errors
                    system_samples += 1
                    writer.write({
                        "schemaVersion": SCHEMA_VERSION,
                        "type": "system",
                        "capturedAt": _captured_at(),
                        "elapsedSeconds": round(elapsed, 3),
                        "devices": result,
                    }, elapsed)
                    next_system = _advance_deadline(
                        next_system, system_interval_seconds, elapsed
                    )
                elapsed = time.monotonic() - started
                next_due = min(next_state, next_groups, next_system)
                sleep_seconds = min(
                    max(0.0, next_due - elapsed),
                    max(0.0, duration_seconds - elapsed),
                    0.25,
                )
                if sleep_seconds > 0:
                    time.sleep(sleep_seconds)
    except KeyboardInterrupt:
        interrupted = True

    elapsed = time.monotonic() - started
    summary = {
        "schemaVersion": SCHEMA_VERSION,
        "type": "summary",
        "capturedAt": _captured_at(),
        "elapsedSeconds": round(elapsed, 3),
        "interrupted": interrupted,
        "stateSamples": state_samples,
        "groupSamples": group_samples,
        "configurationSamples": configuration_samples,
        "systemSamples": system_samples,
        "requestErrors": request_errors,
    }
    writer.write(summary, elapsed, allow_rotate=False)
    writer.close()
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Capture compact local-target, fusion, Group, and resource data "
            "from up to seven NAMO devices without writing to the devices."
        )
    )
    parser.add_argument(
        "--device",
        action="append",
        required=True,
        help="Device URL or alias=URL. Repeat for each device.",
    )
    duration = parser.add_mutually_exclusive_group()
    duration.add_argument("--duration-hours", type=float, default=None)
    duration.add_argument("--duration-seconds", type=float, default=None)
    parser.add_argument(
        "--state-interval-seconds",
        type=float,
        default=DEFAULT_STATE_INTERVAL_SECONDS,
    )
    parser.add_argument(
        "--group-interval-seconds",
        type=float,
        default=DEFAULT_GROUP_INTERVAL_SECONDS,
    )
    parser.add_argument(
        "--system-interval-seconds",
        type=float,
        default=DEFAULT_SYSTEM_INTERVAL_SECONDS,
    )
    parser.add_argument("--timeout-seconds", type=float, default=5.0)
    parser.add_argument(
        "--rotate-seconds", type=float, default=DEFAULT_ROTATE_SECONDS
    )
    parser.add_argument(
        "--output-dir", type=Path, default=Path("captures/office-noise")
    )
    parser.add_argument("--prefix", default="office-noise")
    args = parser.parse_args()

    try:
        devices = parse_devices(args.device)
        duration_seconds = (
            args.duration_seconds
            if args.duration_seconds is not None
            else (args.duration_hours if args.duration_hours is not None else 8.0)
            * 3600.0
        )
        if duration_seconds <= 0:
            raise ValueError("duration must be positive")
        if args.state_interval_seconds < 0.5:
            raise ValueError("state interval must be at least 0.5 seconds")
        if args.group_interval_seconds < 5.0:
            raise ValueError("Group interval must be at least 5 seconds")
        if args.system_interval_seconds < 30.0:
            raise ValueError("system interval must be at least 30 seconds")
        if args.timeout_seconds <= 0 or args.timeout_seconds > 30:
            raise ValueError("timeout must be between 0 and 30 seconds")
        if args.rotate_seconds < 60:
            raise ValueError("rotation interval must be at least 60 seconds")
        writer = RotatingJsonlWriter(
            args.output_dir, args.prefix, args.rotate_seconds
        )
        report = run_capture(
            devices,
            duration_seconds=duration_seconds,
            state_interval_seconds=args.state_interval_seconds,
            group_interval_seconds=args.group_interval_seconds,
            system_interval_seconds=args.system_interval_seconds,
            timeout_seconds=args.timeout_seconds,
            writer=writer,
        )
    except (OSError, ValueError) as error:
        print(f"office_noise_capture: error: {error}", flush=True)
        return 2

    print(json.dumps(report, indent=2), flush=True)
    print("office_noise_capture: complete", flush=True)
    return 130 if report["interrupted"] else 0


if __name__ == "__main__":
    raise SystemExit(main())

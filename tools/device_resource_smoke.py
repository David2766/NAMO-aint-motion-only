#!/usr/bin/env python3
"""Bounded read-only resource smoke for a running NAMO device."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import asdict, dataclass, replace
from datetime import datetime, timezone
import json
from pathlib import Path
import sys
import time
from typing import Any, Protocol
import urllib.error
import urllib.request


READ_ONLY_ENDPOINTS = (
    "/api/state",
    "/api/config",
    "/api/platform/participants",
    "/api/platform/groups",
    "/api/platform/site-configuration",
    "/api/floorplan/status",
)


@dataclass(frozen=True)
class ResourceLimits:
    min_internal_free_bytes: int = 32 * 1024
    min_internal_largest_block_bytes: int = 16 * 1024
    min_httpd_stack_free_bytes: int = 2 * 1024
    min_firmware_headroom_bytes: int = 512 * 1024
    min_storage_headroom_bytes: int = 128 * 1024
    max_internal_free_drop_bytes: int = 32 * 1024
    max_request_errors: int = 0


@dataclass(frozen=True)
class ResourceSnapshot:
    firmware_version: str
    uptime_seconds: int
    internal_free_bytes: int
    internal_min_free_bytes: int
    internal_largest_free_block_bytes: int
    httpd_stack_min_free_bytes: int
    httpd_stack_lowest_endpoint: str | None
    httpd_stack_lowest_phase: str | None
    httpd_stack_lowest_endpoint_free_bytes: int | None
    psram_total_bytes: int
    psram_free_bytes: int
    firmware_used_bytes: int
    firmware_slot_bytes: int
    storage_used_bytes: int
    storage_total_bytes: int
    internal_min_free_source: str | None = None
    internal_min_free_phase: str | None = None
    internal_min_free_observed_at_ms: int | None = None
    internal_min_free_event_count: int | None = None
    internal_min_largest_free_block_bytes: int | None = None
    internal_threshold_source: str | None = None
    internal_threshold_phase: str | None = None
    internal_threshold_free_bytes: int | None = None
    internal_threshold_observed_at_ms: int | None = None
    elapsed_seconds: float = 0.0


class JsonClient(Protocol):
    def get_json(self, path: str) -> dict[str, Any]: ...


class HttpDeviceClient:
    def __init__(self, device: str, timeout: float = 5.0) -> None:
        self.base_url = device.rstrip("/")
        self.timeout = timeout

    def get_json(self, path: str) -> dict[str, Any]:
        request = urllib.request.Request(
            f"{self.base_url}{path}",
            headers={"Accept": "application/json", "Cache-Control": "no-cache"},
            method="GET",
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                status = response.status
                payload = response.read()
        except urllib.error.HTTPError as error:
            detail = error.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"GET {path} failed: status={error.code} body={detail[:160]}") from error
        except urllib.error.URLError as error:
            raise RuntimeError(f"GET {path} failed: {error.reason}") from error
        except (TimeoutError, OSError) as error:
            raise RuntimeError(f"GET {path} failed: {error}") from error
        if status != 200:
            raise RuntimeError(f"GET {path} failed: status={status}")
        try:
            body = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise RuntimeError(f"GET {path} returned invalid JSON") from error
        if not isinstance(body, dict):
            raise RuntimeError(f"GET {path} returned a non-object JSON payload")
        return body


def _required_object(parent: dict[str, Any], key: str) -> dict[str, Any]:
    value = parent.get(key)
    if not isinstance(value, dict):
        raise ValueError(f"missing object: {key}")
    return value


def _required_int(parent: dict[str, Any], key: str, path: str) -> int:
    value = parent.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"missing non-negative integer: {path}.{key}")
    return value


def _optional_int(parent: dict[str, Any], key: str, path: str) -> int | None:
    value = parent.get(key)
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"invalid non-negative integer: {path}.{key}")
    return value


def _optional_string(parent: dict[str, Any], key: str, path: str) -> str | None:
    value = parent.get(key)
    if value is None:
        return None
    if not isinstance(value, str) or not value:
        raise ValueError(f"invalid non-empty string: {path}.{key}")
    return value


def parse_resource_snapshot(status: dict[str, Any]) -> ResourceSnapshot:
    if status.get("ok") is not True:
        raise ValueError("system status is not ok")
    firmware = _required_object(status, "firmware")
    memory = _required_object(status, "memory")
    flash = _required_object(status, "flash")
    version = firmware.get("version")
    if not isinstance(version, str) or not version:
        raise ValueError("missing string: firmware.version")
    return ResourceSnapshot(
        firmware_version=version,
        uptime_seconds=_required_int(firmware, "uptimeSeconds", "firmware"),
        internal_free_bytes=_required_int(memory, "internalFreeBytes", "memory"),
        internal_min_free_bytes=_required_int(memory, "internalMinFreeBytes", "memory"),
        internal_largest_free_block_bytes=_required_int(
            memory, "internalLargestFreeBlockBytes", "memory"
        ),
        httpd_stack_min_free_bytes=_required_int(
            memory, "httpdStackMinFreeBytes", "memory"
        ),
        httpd_stack_lowest_endpoint=_optional_string(
            memory, "httpdStackLowestEndpoint", "memory"
        ),
        httpd_stack_lowest_phase=_optional_string(
            memory, "httpdStackLowestPhase", "memory"
        ),
        httpd_stack_lowest_endpoint_free_bytes=_optional_int(
            memory, "httpdStackLowestEndpointFreeBytes", "memory"
        ),
        psram_total_bytes=_required_int(memory, "psramTotal", "memory"),
        psram_free_bytes=_required_int(memory, "psramFree", "memory"),
        firmware_used_bytes=_required_int(flash, "firmwareUsedBytes", "flash"),
        firmware_slot_bytes=_required_int(flash, "firmwareSlotBytes", "flash"),
        storage_used_bytes=_required_int(flash, "storageUsedBytes", "flash"),
        storage_total_bytes=_required_int(flash, "storageTotalBytes", "flash"),
        internal_min_free_source=_optional_string(
            memory, "internalMinFreeSource", "memory"
        ),
        internal_min_free_phase=_optional_string(
            memory, "internalMinFreePhase", "memory"
        ),
        internal_min_free_observed_at_ms=_optional_int(
            memory, "internalMinFreeObservedAtMs", "memory"
        ),
        internal_min_free_event_count=_optional_int(
            memory, "internalMinFreeEventCount", "memory"
        ),
        internal_min_largest_free_block_bytes=_optional_int(
            memory, "internalMinLargestFreeBlockBytes", "memory"
        ),
        internal_threshold_source=_optional_string(
            memory, "internalThresholdSource", "memory"
        ),
        internal_threshold_phase=_optional_string(
            memory, "internalThresholdPhase", "memory"
        ),
        internal_threshold_free_bytes=_optional_int(
            memory, "internalThresholdFreeBytes", "memory"
        ),
        internal_threshold_observed_at_ms=_optional_int(
            memory, "internalThresholdObservedAtMs", "memory"
        ),
    )


def evaluate_resource_samples(
    samples: list[ResourceSnapshot], limits: ResourceLimits,
    *, scope: str = "lifecycle",
    request_errors: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    if not samples:
        raise ValueError("at least one resource sample is required")
    versions = {sample.firmware_version for sample in samples}
    if len(versions) != 1:
        raise ValueError("firmware version changed during resource smoke")
    if scope not in ("lifecycle", "window"):
        raise ValueError("scope must be lifecycle or window")

    errors = request_errors or []
    lifetime_min_start = samples[0].internal_min_free_bytes
    lifetime_min_end = min(
        sample.internal_min_free_bytes for sample in samples
    )
    new_lifetime_low = lifetime_min_end < lifetime_min_start
    sampled_current_min = min(
        sample.internal_free_bytes for sample in samples
    )
    window_internal_min = min(
        sampled_current_min,
        lifetime_min_end if new_lifetime_low else sampled_current_min,
    )
    unexpected_reboot = any(
        current.uptime_seconds < previous.uptime_seconds
        for previous, current in zip(samples, samples[1:])
    )

    observed = {
        "internalMinFreeBytes": min(sample.internal_min_free_bytes for sample in samples),
        "lifetimeMinFreeStartBytes": lifetime_min_start,
        "lifetimeMinFreeEndBytes": lifetime_min_end,
        "newLifetimeLow": new_lifetime_low,
        "sampledCurrentMinFreeBytes": sampled_current_min,
        "windowInternalMinFreeBytes": window_internal_min,
        "internalLargestFreeBlockBytes": min(
            sample.internal_largest_free_block_bytes for sample in samples
        ),
        "httpdStackMinFreeBytes": min(
            sample.httpd_stack_min_free_bytes for sample in samples
        ),
        "firmwareHeadroomBytes": min(
            sample.firmware_slot_bytes - sample.firmware_used_bytes for sample in samples
        ),
        "storageHeadroomBytes": min(
            sample.storage_total_bytes - sample.storage_used_bytes for sample in samples
        ),
        "internalFreeDropBytes": max(
            0,
            samples[0].internal_free_bytes
            - min(sample.internal_free_bytes for sample in samples),
        ),
        "psramFreeBytes": min(sample.psram_free_bytes for sample in samples),
        "requestErrorCount": len(errors),
        "unexpectedReboot": unexpected_reboot,
    }
    attributed_samples = [
        sample
        for sample in samples
        if sample.httpd_stack_lowest_endpoint is not None
        and sample.httpd_stack_lowest_endpoint_free_bytes is not None
    ]
    if attributed_samples:
        lowest_attributed = min(
            attributed_samples,
            key=lambda sample: sample.httpd_stack_lowest_endpoint_free_bytes,
        )
        observed["httpdStackLowestEndpoint"] = (
            lowest_attributed.httpd_stack_lowest_endpoint
        )
        observed["httpdStackLowestPhase"] = (
            lowest_attributed.httpd_stack_lowest_phase
        )
        observed["httpdStackLowestEndpointFreeBytes"] = (
            lowest_attributed.httpd_stack_lowest_endpoint_free_bytes
        )

    attributed_internal = [
        sample for sample in samples
        if sample.internal_min_free_source is not None
        and sample.internal_min_free_event_count is not None
    ]
    if attributed_internal:
        latest_attribution = max(
            attributed_internal,
            key=lambda sample: sample.internal_min_free_event_count or 0,
        )
        observed["internalMinFreeSource"] = (
            latest_attribution.internal_min_free_source
        )
        observed["internalMinFreePhase"] = (
            latest_attribution.internal_min_free_phase
        )
        observed["internalMinFreeObservedAtMs"] = (
            latest_attribution.internal_min_free_observed_at_ms
        )
        observed["internalMinFreeEventCount"] = (
            latest_attribution.internal_min_free_event_count
        )
        observed["internalMinLargestFreeBlockBytes"] = (
            latest_attribution.internal_min_largest_free_block_bytes
        )

    threshold_samples = [
        sample for sample in samples
        if sample.internal_threshold_source is not None
        and sample.internal_threshold_free_bytes is not None
    ]
    if threshold_samples:
        threshold = min(
            threshold_samples,
            key=lambda sample: sample.internal_threshold_observed_at_ms or 0,
        )
        observed["internalThresholdSource"] = (
            threshold.internal_threshold_source
        )
        observed["internalThresholdPhase"] = (
            threshold.internal_threshold_phase
        )
        observed["internalThresholdFreeBytes"] = (
            threshold.internal_threshold_free_bytes
        )
        observed["internalThresholdObservedAtMs"] = (
            threshold.internal_threshold_observed_at_ms
        )

    internal_check_name = (
        "internal_min_free" if scope == "lifecycle"
        else "window_internal_min_free"
    )
    internal_check_value = (
        observed["internalMinFreeBytes"] if scope == "lifecycle"
        else observed["windowInternalMinFreeBytes"]
    )
    specifications = (
        (internal_check_name, internal_check_value, ">=", limits.min_internal_free_bytes, "bytes"),
        (
            "internal_largest_free_block",
            observed["internalLargestFreeBlockBytes"],
            ">=",
            limits.min_internal_largest_block_bytes,
            "bytes",
        ),
        ("httpd_stack_min_free", observed["httpdStackMinFreeBytes"], ">=", limits.min_httpd_stack_free_bytes, "bytes"),
        ("firmware_headroom", observed["firmwareHeadroomBytes"], ">=", limits.min_firmware_headroom_bytes, "bytes"),
        ("storage_headroom", observed["storageHeadroomBytes"], ">=", limits.min_storage_headroom_bytes, "bytes"),
        ("internal_free_drop", observed["internalFreeDropBytes"], "<=", limits.max_internal_free_drop_bytes, "bytes"),
        ("request_errors", len(errors), "<=", limits.max_request_errors, "count"),
        ("unexpected_reboot", int(unexpected_reboot), "<=", 0, "count"),
    )
    checks = []
    for name, value, operator, limit, unit in specifications:
        passed = value >= limit if operator == ">=" else value <= limit
        checks.append({
            "name": name,
            "value": value,
            "operator": operator,
            "limit": limit,
            "passed": passed,
            "unit": unit,
        })
    return {
        "schemaVersion": 2,
        "capturedAt": datetime.now(timezone.utc).isoformat(),
        "firmwareVersion": samples[0].firmware_version,
        "sampleCount": len(samples),
        "scope": scope,
        "limits": asdict(limits),
        "observed": observed,
        "checks": checks,
        "requestErrors": errors,
        "timeline": [
            {
                "elapsedSeconds": round(sample.elapsed_seconds, 3),
                "uptimeSeconds": sample.uptime_seconds,
                "internalFreeBytes": sample.internal_free_bytes,
                "internalMinFreeBytes": sample.internal_min_free_bytes,
                "internalLargestFreeBlockBytes":
                    sample.internal_largest_free_block_bytes,
                "httpdStackMinFreeBytes": sample.httpd_stack_min_free_bytes,
            }
            for sample in samples
        ],
        "passed": all(check["passed"] for check in checks),
    }


def _exercise_read_only_endpoints(
    client: JsonClient, concurrency: int, elapsed_seconds: float
) -> list[dict[str, Any]]:
    errors: list[dict[str, Any]] = []
    with ThreadPoolExecutor(max_workers=concurrency) as executor:
        futures = {
            executor.submit(client.get_json, path): path
            for path in READ_ONLY_ENDPOINTS
        }
        for future in as_completed(futures):
            path = futures[future]
            try:
                future.result()
            except Exception as error:
                errors.append({
                    "elapsedSeconds": round(elapsed_seconds, 3),
                    "endpoint": path,
                    "error": str(error),
                })
    return errors


def run_smoke(
    client: JsonClient,
    limits: ResourceLimits,
    rounds: int = 3,
    concurrency: int = 4,
    interval_seconds: float = 0.25,
    exercise: bool = True,
    scope: str = "lifecycle",
    duration_seconds: float | None = None,
) -> dict[str, Any]:
    if rounds < 1:
        raise ValueError("rounds must be at least 1")
    if concurrency < 1 or concurrency > 8:
        raise ValueError("concurrency must be between 1 and 8")
    if duration_seconds is not None and duration_seconds <= 0:
        raise ValueError("duration_seconds must be positive")

    started = time.monotonic()
    samples = [replace(
        parse_resource_snapshot(client.get_json("/api/system/status")),
        elapsed_seconds=0.0,
    )]
    request_errors: list[dict[str, Any]] = []
    round_index = 0
    while round_index < rounds or duration_seconds is not None:
        elapsed = time.monotonic() - started
        if duration_seconds is not None and elapsed >= duration_seconds:
            break
        if exercise:
            request_errors.extend(
                _exercise_read_only_endpoints(client, concurrency, elapsed)
            )
        if interval_seconds > 0:
            time.sleep(interval_seconds)
        try:
            samples.append(replace(
                parse_resource_snapshot(client.get_json("/api/system/status")),
                elapsed_seconds=time.monotonic() - started,
            ))
        except Exception as error:
            request_errors.append({
                "elapsedSeconds": round(time.monotonic() - started, 3),
                "endpoint": "/api/system/status",
                "error": str(error),
            })
        round_index += 1

    report = evaluate_resource_samples(
        samples, limits, scope=scope, request_errors=request_errors
    )
    report["exercise"] = {
        "enabled": exercise,
        "rounds": rounds,
        "concurrency": concurrency,
        "readRequests": round_index * len(READ_ONLY_ENDPOINTS) if exercise else 0,
        "completedRounds": round_index,
        "durationSeconds": round(time.monotonic() - started, 3),
    }
    return report


def _format_bytes(value: int) -> str:
    return f"{value / 1024:.1f} KiB"


def print_report(report: dict[str, Any]) -> None:
    print(
        f"device_resource_smoke: firmware={report['firmwareVersion']} "
        f"samples={report['sampleCount']} scope={report['scope']}"
    )
    for check in report["checks"]:
        result = "PASS" if check["passed"] else "FAIL"
        if check.get("unit") == "bytes":
            value = _format_bytes(check["value"])
            limit = _format_bytes(check["limit"])
        else:
            value = str(check["value"])
            limit = str(check["limit"])
        print(f"  {result} {check['name']}: {value} "
              f"{check['operator']} {limit}")
    observed = report["observed"]
    if (report["scope"] == "window" and
            observed["lifetimeMinFreeStartBytes"] <
            report["limits"]["min_internal_free_bytes"] and
            not observed["newLifetimeLow"]):
        print(
            "  WARN historical_min_preexisting: "
            f"{_format_bytes(observed['lifetimeMinFreeStartBytes'])}; "
            "no newer lifetime low was observed"
        )
    endpoint = report["observed"].get("httpdStackLowestEndpoint")
    endpoint_free = report["observed"].get(
        "httpdStackLowestEndpointFreeBytes"
    )
    if endpoint is not None and endpoint_free is not None:
        phase = report["observed"].get("httpdStackLowestPhase")
        phase_suffix = f" [{phase}]" if phase is not None else ""
        print(
            f"  INFO httpd_stack_lowest_endpoint: {endpoint}{phase_suffix} "
            f"({_format_bytes(endpoint_free)} free)"
        )
    source = observed.get("internalMinFreeSource")
    if source is not None:
        phase = observed.get("internalMinFreePhase") or "unknown"
        at_ms = observed.get("internalMinFreeObservedAtMs")
        at_text = f" at {at_ms}ms" if at_ms is not None else ""
        print(
            "  INFO internal_min_free_attribution: "
            f"{source} [{phase}]{at_text}"
        )
    threshold_source = observed.get("internalThresholdSource")
    if threshold_source is not None:
        threshold_phase = observed.get("internalThresholdPhase") or "unknown"
        threshold_at_ms = observed.get("internalThresholdObservedAtMs")
        threshold_at_text = (
            f" at {threshold_at_ms}ms" if threshold_at_ms is not None else ""
        )
        threshold_free = observed.get("internalThresholdFreeBytes")
        threshold_free_text = (
            f" ({_format_bytes(threshold_free)} free)"
            if threshold_free is not None else ""
        )
        print(
            "  INFO internal_48k_first_crossing: "
            f"{threshold_source} [{threshold_phase}]"
            f"{threshold_at_text}{threshold_free_text}"
        )
    if report["requestErrors"]:
        for error in report["requestErrors"][:5]:
            print(
                "  ERROR request: "
                f"{error['endpoint']} at {error['elapsedSeconds']}s: "
                f"{error['error']}"
            )
        remaining = len(report["requestErrors"]) - 5
        if remaining > 0:
            print(f"  ERROR request: {remaining} more errors in report")
    print("device_resource_smoke: ok" if report["passed"] else "device_resource_smoke: failed")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, help="device base URL, for example http://192.168.29.104")
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--concurrency", type=int, default=4)
    parser.add_argument("--interval", type=float, default=0.25)
    parser.add_argument(
        "--mode", choices=("stress", "observe"), default="stress",
        help="stress exercises read-only endpoints; observe only samples status",
    )
    parser.add_argument(
        "--scope", choices=("lifecycle", "window"), default="lifecycle",
        help="lifecycle evaluates the boot lifetime minimum; window evaluates this run",
    )
    parser.add_argument("--duration-seconds", type=float)
    parser.add_argument("--no-exercise", action="store_true")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--min-internal-free", type=int, default=32 * 1024)
    parser.add_argument("--min-largest-block", type=int, default=16 * 1024)
    parser.add_argument("--min-httpd-stack", type=int, default=2 * 1024)
    parser.add_argument("--min-firmware-headroom", type=int, default=512 * 1024)
    parser.add_argument("--min-storage-headroom", type=int, default=128 * 1024)
    parser.add_argument("--max-internal-drop", type=int, default=32 * 1024)
    parser.add_argument("--max-request-errors", type=int, default=0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    limits = ResourceLimits(
        min_internal_free_bytes=args.min_internal_free,
        min_internal_largest_block_bytes=args.min_largest_block,
        min_httpd_stack_free_bytes=args.min_httpd_stack,
        min_firmware_headroom_bytes=args.min_firmware_headroom,
        min_storage_headroom_bytes=args.min_storage_headroom,
        max_internal_free_drop_bytes=args.max_internal_drop,
        max_request_errors=args.max_request_errors,
    )
    try:
        report = run_smoke(
            HttpDeviceClient(args.device, timeout=args.timeout),
            limits,
            rounds=args.rounds,
            concurrency=args.concurrency,
            interval_seconds=args.interval,
            exercise=args.mode == "stress" and not args.no_exercise,
            scope=args.scope,
            duration_seconds=args.duration_seconds,
        )
        report["mode"] = (
            "stress" if args.mode == "stress" and not args.no_exercise
            else "observe"
        )
    except (RuntimeError, ValueError) as error:
        print(f"device_resource_smoke: error: {error}", file=sys.stderr)
        return 2
    print_report(report)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

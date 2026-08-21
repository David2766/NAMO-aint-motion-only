#!/usr/bin/env python3
"""Read-only comparison of production state and two-Node Shadow Fusion."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import json
from pathlib import Path
import time
from typing import Any, Protocol
import urllib.error
import urllib.request


@dataclass(frozen=True)
class ProductionTarget:
    target_id: str
    x_mm: float
    y_mm: float


@dataclass(frozen=True)
class ShadowTrack:
    track_id: int
    state: str
    source_count: int
    confidence: int
    correlated_views: bool
    x_mm: float
    y_mm: float
    vx_mm_s: float
    vy_mm_s: float
    variance_x_mm2: float
    variance_y_mm2: float


@dataclass(frozen=True)
class ShadowDiagnostics:
    valid_inputs: int
    candidate_pairs: int
    paired_measurements: int
    ambiguous_pairs: int
    dropped_measurements: int
    tentative_tracks: int
    confirmed_tracks: int
    coasting_tracks: int


@dataclass(frozen=True)
class ComparisonSample:
    schema_version: int
    captured_at: str
    production_presence: bool
    production_target_count: int
    production_targets: tuple[ProductionTarget, ...]
    shadow_state: str
    shadow_presence: bool
    shadow_target_count: int
    shadow_track_count: int
    shadow_tracks: tuple[ShadowTrack, ...]
    shadow_diagnostics: ShadowDiagnostics
    target_count_delta: int
    mismatch_kind: str
    overcount_reason: str | None
    admitted_single_source_tracks: int
    admitted_multi_source_tracks: int
    max_shadow_track_separation_mm: float | None
    max_cycle_us: int
    presence_mismatch: bool
    target_count_mismatch: bool


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
            raise RuntimeError(
                f"GET {path} failed: status={error.code} body={detail[:160]}"
            ) from error
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


def _required_bool(parent: dict[str, Any], key: str, path: str) -> bool:
    value = parent.get(key)
    if not isinstance(value, bool):
        raise ValueError(f"missing boolean: {path}.{key}")
    return value


def _required_int(parent: dict[str, Any], key: str, path: str) -> int:
    value = parent.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"missing non-negative integer: {path}.{key}")
    return value


def _required_number(
    parent: dict[str, Any], key: str, path: str
) -> float:
    value = parent.get(key)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"missing number: {path}.{key}")
    return float(value)


def _required_string(
    parent: dict[str, Any], key: str, path: str
) -> str:
    value = parent.get(key)
    if not isinstance(value, str):
        raise ValueError(f"missing string: {path}.{key}")
    return value


def _parse_production_targets(
    state: dict[str, Any],
) -> tuple[ProductionTarget, ...]:
    targets = state.get("targets")
    if not isinstance(targets, list):
        raise ValueError("missing list: state.targets")
    result: list[ProductionTarget] = []
    for index, target in enumerate(targets):
        path = f"state.targets[{index}]"
        if not isinstance(target, dict):
            raise ValueError(f"invalid object: {path}")
        if not _required_bool(target, "active", path):
            continue
        result.append(
            ProductionTarget(
                target_id=_required_string(target, "id", path),
                x_mm=_required_number(target, "x", path),
                y_mm=_required_number(target, "y", path),
            )
        )
    return tuple(result)


def _parse_shadow_track(
    track: dict[str, Any], index: int
) -> ShadowTrack:
    path = f"shadowFusion.tracks[{index}]"
    return ShadowTrack(
        track_id=_required_int(track, "id", path),
        state=_required_string(track, "state", path),
        source_count=_required_int(track, "sourceCount", path),
        confidence=_required_int(track, "confidence", path),
        correlated_views=_required_bool(track, "correlatedViews", path),
        x_mm=_required_number(track, "xMm", path),
        y_mm=_required_number(track, "yMm", path),
        vx_mm_s=_required_number(track, "vxMmS", path),
        vy_mm_s=_required_number(track, "vyMmS", path),
        variance_x_mm2=_required_number(
            track, "varianceXMillimeters2", path
        ),
        variance_y_mm2=_required_number(
            track, "varianceYMillimeters2", path
        ),
    )


def _parse_shadow_diagnostics(
    shadow: dict[str, Any], required: bool,
) -> ShadowDiagnostics:
    diagnostics = shadow.get("diagnostics")
    if not isinstance(diagnostics, dict):
        if required:
            raise ValueError("missing object: shadowFusion.diagnostics")
        return ShadowDiagnostics(0, 0, 0, 0, 0, 0, 0, 0)
    path = "shadowFusion.diagnostics"
    return ShadowDiagnostics(
        valid_inputs=_required_int(diagnostics, "validInputs", path),
        candidate_pairs=_required_int(diagnostics, "candidatePairs", path),
        paired_measurements=_required_int(
            diagnostics, "pairedMeasurements", path
        ),
        ambiguous_pairs=_required_int(diagnostics, "ambiguousPairs", path),
        dropped_measurements=_required_int(
            diagnostics, "droppedMeasurements", path
        ),
        tentative_tracks=_required_int(
            diagnostics, "tentativeTracks", path
        ),
        confirmed_tracks=_required_int(
            diagnostics, "confirmedTracks", path
        ),
        coasting_tracks=_required_int(
            diagnostics, "coastingTracks", path
        ),
    )


def _overcount_reason(diagnostics: ShadowDiagnostics) -> str:
    if diagnostics.paired_measurements > 0:
        return "lifecycle_overlap"
    if diagnostics.ambiguous_pairs > 0:
        return "ambiguous_pair"
    if diagnostics.candidate_pairs == 0:
        return "no_pair_candidate"
    return "unpaired_candidate"


def _max_track_separation(tracks: list[ShadowTrack]) -> float | None:
    if len(tracks) < 2:
        return None
    maximum = 0.0
    for first_index, first in enumerate(tracks):
        for second in tracks[first_index + 1 :]:
            maximum = max(
                maximum,
                ((first.x_mm - second.x_mm) ** 2
                 + (first.y_mm - second.y_mm) ** 2) ** 0.5,
            )
    return maximum


def parse_sample(
    state: dict[str, Any],
    groups: dict[str, Any],
    captured_at: str | None = None,
) -> ComparisonSample:
    if state.get("connected") is not True:
        raise ValueError("production state is not connected")
    shadow = groups.get("shadowFusion")
    if not isinstance(shadow, dict):
        raise ValueError("missing object: shadowFusion")
    shadow_state = shadow.get("state")
    if not isinstance(shadow_state, str) or not shadow_state:
        raise ValueError("missing string: shadowFusion.state")
    tracks = shadow.get("tracks")
    if not isinstance(tracks, list):
        raise ValueError("missing list: shadowFusion.tracks")

    admitted_states = {"confirmed", "coasting"}
    parsed_tracks: list[ShadowTrack] = []
    for index, track in enumerate(tracks):
        if not isinstance(track, dict):
            raise ValueError(f"invalid object: shadowFusion.tracks[{index}]")
        parsed_tracks.append(_parse_shadow_track(track, index))

    admitted_tracks = [
        track for track in parsed_tracks if track.state in admitted_states
    ]
    shadow_target_count = len(admitted_tracks)
    production_presence = _required_bool(state, "presence", "state")
    production_target_count = _required_int(state, "targetCount", "state")
    shadow_presence = shadow_target_count > 0
    target_count_delta = shadow_target_count - production_target_count
    if target_count_delta > 0:
        mismatch_kind = "overcount"
    elif target_count_delta < 0:
        mismatch_kind = "undercount"
    else:
        mismatch_kind = "match"
    diagnostics = _parse_shadow_diagnostics(
        shadow, required=shadow_state == "active"
    )
    return ComparisonSample(
        schema_version=2,
        captured_at=captured_at
        or datetime.now(timezone.utc).isoformat(),
        production_presence=production_presence,
        production_target_count=production_target_count,
        production_targets=_parse_production_targets(state),
        shadow_state=shadow_state,
        shadow_presence=shadow_presence,
        shadow_target_count=shadow_target_count,
        shadow_track_count=_required_int(
            shadow, "trackCount", "shadowFusion"
        ),
        shadow_tracks=tuple(parsed_tracks),
        shadow_diagnostics=diagnostics,
        target_count_delta=target_count_delta,
        mismatch_kind=mismatch_kind,
        overcount_reason=(
            _overcount_reason(diagnostics)
            if target_count_delta > 0
            else None
        ),
        admitted_single_source_tracks=sum(
            track.source_count == 1 for track in admitted_tracks
        ),
        admitted_multi_source_tracks=sum(
            track.source_count > 1 for track in admitted_tracks
        ),
        max_shadow_track_separation_mm=_max_track_separation(
            admitted_tracks
        ),
        max_cycle_us=_required_int(shadow, "maxCycleUs", "shadowFusion"),
        presence_mismatch=production_presence != shadow_presence,
        target_count_mismatch=production_target_count != shadow_target_count,
    )


def _captured_timestamp(sample: ComparisonSample) -> float:
    return datetime.fromisoformat(
        sample.captured_at.replace("Z", "+00:00")
    ).timestamp()


def _overcount_episode_summary(
    samples: list[ComparisonSample],
) -> tuple[int, int, float]:
    episode_count = 0
    longest_samples = 0
    longest_seconds = 0.0
    current: list[ComparisonSample] = []

    def finish_episode() -> None:
        nonlocal episode_count, longest_samples, longest_seconds, current
        if not current:
            return
        episode_count += 1
        longest_samples = max(longest_samples, len(current))
        duration = max(
            0.0,
            _captured_timestamp(current[-1])
            - _captured_timestamp(current[0]),
        )
        longest_seconds = max(longest_seconds, duration)
        current = []

    for sample in samples:
        if sample.shadow_state == "active" and sample.target_count_delta > 0:
            current.append(sample)
        else:
            finish_episode()
    finish_episode()
    return episode_count, longest_samples, longest_seconds


def summarize_samples(samples: list[ComparisonSample]) -> dict[str, Any]:
    if not samples:
        raise ValueError("at least one comparison sample is required")
    active = [sample for sample in samples if sample.shadow_state == "active"]
    presence_mismatches = sum(sample.presence_mismatch for sample in active)
    target_mismatches = sum(
        sample.target_count_mismatch for sample in active
    )
    active_count = len(active)
    overcount_samples = sum(sample.target_count_delta > 0 for sample in active)
    undercount_samples = sum(
        sample.target_count_delta < 0 for sample in active
    )
    exact_count_samples = active_count - overcount_samples - undercount_samples
    episode_count, longest_samples, longest_seconds = (
        _overcount_episode_summary(samples)
    )
    overcount_reasons = {
        reason: sum(
            sample.overcount_reason == reason for sample in active
        )
        for reason in (
            "no_pair_candidate",
            "ambiguous_pair",
            "unpaired_candidate",
            "lifecycle_overlap",
        )
    }
    return {
        "schemaVersion": 2,
        "capturedAt": datetime.now(timezone.utc).isoformat(),
        "sampleCount": len(samples),
        "activeShadowSamples": active_count,
        "inactiveShadowSamples": len(samples) - active_count,
        "presenceMismatches": presence_mismatches,
        "presenceMismatchRate": (
            presence_mismatches / active_count if active_count else None
        ),
        "targetCountMismatches": target_mismatches,
        "targetCountMismatchRate": (
            target_mismatches / active_count if active_count else None
        ),
        "exactTargetCountSamples": exact_count_samples,
        "overcountSamples": overcount_samples,
        "overcountRate": (
            overcount_samples / active_count if active_count else None
        ),
        "undercountSamples": undercount_samples,
        "undercountRate": (
            undercount_samples / active_count if active_count else None
        ),
        "overcountEpisodes": episode_count,
        "longestOvercountEpisodeSamples": longest_samples,
        "longestOvercountEpisodeSeconds": longest_seconds,
        "overcountReasonSamples": overcount_reasons,
        "maxShadowTrackSeparationMm": max(
            (
                sample.max_shadow_track_separation_mm
                for sample in active
                if sample.max_shadow_track_separation_mm is not None
            ),
            default=None,
        ),
        "maxCycleUs": max(sample.max_cycle_us for sample in samples),
    }


def run_comparison(
    client: JsonClient,
    duration_seconds: float,
    interval_seconds: float,
    output: Path | None = None,
) -> dict[str, Any]:
    if duration_seconds <= 0 or duration_seconds > 3600:
        raise ValueError("duration must be between 0 and 3600 seconds")
    if interval_seconds < 0.25 or interval_seconds > 60:
        raise ValueError("interval must be between 0.25 and 60 seconds")

    samples: list[ComparisonSample] = []
    deadline = time.monotonic() + duration_seconds
    output_file = output.open("w", encoding="utf-8") if output else None
    try:
        while True:
            sample = parse_sample(
                client.get_json("/api/state"),
                client.get_json("/api/platform/groups"),
            )
            samples.append(sample)
            if output_file is not None:
                output_file.write(
                    json.dumps(asdict(sample), separators=(",", ":")) + "\n"
                )
                output_file.flush()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            time.sleep(min(interval_seconds, remaining))
    finally:
        if output_file is not None:
            output_file.close()
    return summarize_samples(samples)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Compare current production presence/target count with read-only "
            "Shadow Fusion output on the elected Group Leader."
        )
    )
    parser.add_argument("--device", required=True, help="Leader base URL")
    parser.add_argument("--duration-seconds", type=float, default=60.0)
    parser.add_argument("--interval-seconds", type=float, default=1.0)
    parser.add_argument(
        "--output",
        type=Path,
        help="Optional JSONL sample path on this computer",
    )
    args = parser.parse_args()
    report = run_comparison(
        HttpDeviceClient(args.device),
        args.duration_seconds,
        args.interval_seconds,
        args.output,
    )
    print(json.dumps(report, indent=2))
    if report["activeShadowSamples"] == 0:
        print("shadow_fusion_compare: no active Shadow samples", flush=True)
        return 2
    print("shadow_fusion_compare: comparison completed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

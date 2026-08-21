#!/usr/bin/env python3
"""Analyze rotated JSONL sessions from office_noise_capture.py."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from datetime import datetime, timezone, tzinfo
import json
import math
from pathlib import Path
import re
import sys
from typing import Any, Iterable


REPORT_SCHEMA_VERSION = 1
CAPTURE_SCHEMA_VERSION = 1
DEFAULT_STATE_INTERVAL_SECONDS = 1.0
MAX_ANOMALY_EXAMPLES = 50
MAX_EVENT_DETAILS = 1000


def parse_timestamp(value: str) -> datetime:
    if not isinstance(value, str) or not value.strip():
        raise ValueError("timestamp must be a non-empty string")
    parsed = datetime.fromisoformat(value.strip().replace("Z", "+00:00"))
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise ValueError(f"timestamp must include a UTC offset: {value}")
    return parsed.astimezone(timezone.utc)


def format_timestamp(
    value: datetime | None,
    display_timezone: tzinfo = timezone.utc,
) -> str | None:
    if value is None:
        return None
    return value.astimezone(display_timezone).isoformat(timespec="milliseconds")


def format_timezone_offset(value: datetime, display_timezone: tzinfo) -> str:
    offset = value.astimezone(display_timezone).utcoffset()
    total_minutes = int((offset.total_seconds() if offset else 0) / 60)
    sign = "+" if total_minutes >= 0 else "-"
    hours, minutes = divmod(abs(total_minutes), 60)
    return f"{sign}{hours:02d}:{minutes:02d}"


def _object(value: Any) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def _array(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def _number(value: Any) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    result = float(value)
    return result if math.isfinite(result) else None


def _counter_dict(counter: Counter[Any]) -> dict[str, int]:
    return {
        str(key): counter[key]
        for key in sorted(counter, key=lambda item: str(item))
    }


def percentile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, math.ceil(fraction * len(ordered)) - 1)
    return round(ordered[index], 3)


def ratio(numerator: int, denominator: int) -> float | None:
    if denominator <= 0:
        return None
    return round(numerator / denominator, 6)


@dataclass(frozen=True)
class LabelInterval:
    name: str
    expected_state: str
    start: datetime | None = None
    end: datetime | None = None
    expected_target_count: int | None = None

    def contains(self, captured_at: datetime) -> bool:
        return (
            (self.start is None or captured_at >= self.start)
            and (self.end is None or captured_at < self.end)
        )


def load_labels(path: Path) -> list[LabelInterval]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"unable to read labels file {path}: {error}") from error
    if not isinstance(payload, dict) or payload.get("schemaVersion") != 1:
        raise ValueError("labels file must use schemaVersion 1")
    intervals: list[LabelInterval] = []
    names: set[str] = set()
    for raw in _array(payload.get("intervals")):
        item = _object(raw)
        name = item.get("name")
        expected_state = item.get("expectedState")
        if not isinstance(name, str) or not name.strip():
            raise ValueError("every label interval requires a name")
        name = name.strip()
        if name in names:
            raise ValueError(f"duplicate label interval name: {name}")
        if expected_state not in ("occupied", "unoccupied", "unknown"):
            raise ValueError(
                f"label {name} expectedState must be occupied, "
                "unoccupied, or unknown"
            )
        start = parse_timestamp(item["start"]) if item.get("start") else None
        end = parse_timestamp(item["end"]) if item.get("end") else None
        if start is not None and end is not None and start >= end:
            raise ValueError(f"label {name} start must be before end")
        expected_target_count = item.get("expectedTargetCount")
        if expected_target_count is None and expected_state == "unoccupied":
            expected_target_count = 0
        if expected_target_count is not None and (
            isinstance(expected_target_count, bool)
            or not isinstance(expected_target_count, int)
            or expected_target_count < 0
            or expected_target_count > 6
        ):
            raise ValueError(
                f"label {name} expectedTargetCount must be between 0 and 6"
            )
        intervals.append(LabelInterval(
            name,
            expected_state,
            start,
            end,
            expected_target_count,
        ))
        names.add(name)
    if not intervals:
        raise ValueError("labels file contains no intervals")
    return intervals


def resolve_input_files(inputs: list[str]) -> list[Path]:
    paths: list[Path] = []
    for value in inputs:
        path = Path(value)
        if path.is_dir():
            paths.extend(sorted(path.glob("*.jsonl")))
        elif path.is_file():
            paths.append(path)
        else:
            import glob

            paths.extend(Path(match) for match in sorted(glob.glob(value)))
    unique = sorted({path.resolve() for path in paths}, key=lambda item: item.name)
    if not unique:
        raise ValueError("no JSONL capture files found")
    session_keys = {
        re.sub(r"-part\d{4}$", "", path.stem)
        for path in unique
    }
    if len(session_keys) != 1:
        sessions = ", ".join(sorted(session_keys))
        raise ValueError(
            "input contains multiple capture sessions; select one session: "
            f"{sessions}"
        )
    return unique


def parse_comparison(value: str) -> tuple[str, str]:
    aliases = [item.strip() for item in value.split(",")]
    if len(aliases) != 2 or not all(aliases):
        raise ValueError("comparison must contain exactly two aliases: A,B")
    if aliases[0] == aliases[1]:
        raise ValueError("comparison aliases must be different")
    for alias in aliases:
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,32}", alias):
            raise ValueError(f"invalid comparison alias: {alias}")
    return aliases[0], aliases[1]


def read_records(paths: list[Path]) -> Iterable[tuple[Path, int, dict[str, Any]]]:
    for path in paths:
        try:
            handle = path.open("r", encoding="utf-8")
        except OSError as error:
            raise ValueError(f"unable to read {path}: {error}") from error
        with handle:
            for line_number, line in enumerate(handle, 1):
                if not line.strip():
                    continue
                try:
                    record = json.loads(line)
                except json.JSONDecodeError as error:
                    raise ValueError(
                        f"invalid JSON at {path}:{line_number}: {error}"
                    ) from error
                if not isinstance(record, dict):
                    raise ValueError(
                        f"record at {path}:{line_number} is not an object"
                    )
                if record.get("schemaVersion") != CAPTURE_SCHEMA_VERSION:
                    raise ValueError(
                        f"unsupported capture schema at {path}:{line_number}"
                    )
                yield path, line_number, record


@dataclass
class CoordinateStats:
    count: int = 0
    sum_x: float = 0.0
    sum_y: float = 0.0
    sum_xx: float = 0.0
    sum_yy: float = 0.0
    min_x: float | None = None
    max_x: float | None = None
    min_y: float | None = None
    max_y: float | None = None

    def add(self, x: Any, y: Any) -> None:
        x_value = _number(x)
        y_value = _number(y)
        if x_value is None or y_value is None:
            return
        self.count += 1
        self.sum_x += x_value
        self.sum_y += y_value
        self.sum_xx += x_value * x_value
        self.sum_yy += y_value * y_value
        self.min_x = x_value if self.min_x is None else min(self.min_x, x_value)
        self.max_x = x_value if self.max_x is None else max(self.max_x, x_value)
        self.min_y = y_value if self.min_y is None else min(self.min_y, y_value)
        self.max_y = y_value if self.max_y is None else max(self.max_y, y_value)

    def report(self) -> dict[str, Any] | None:
        if self.count == 0:
            return None
        mean_x = self.sum_x / self.count
        mean_y = self.sum_y / self.count
        variance_x = max(0.0, self.sum_xx / self.count - mean_x * mean_x)
        variance_y = max(0.0, self.sum_yy / self.count - mean_y * mean_y)
        return {
            "sampleCount": self.count,
            "meanX": round(mean_x, 1),
            "meanY": round(mean_y, 1),
            "standardDeviationX": round(math.sqrt(variance_x), 1),
            "standardDeviationY": round(math.sqrt(variance_y), 1),
            "radialRmsSpread": round(math.sqrt(variance_x + variance_y), 1),
            "minX": self.min_x,
            "maxX": self.max_x,
            "minY": self.min_y,
            "maxY": self.max_y,
        }


@dataclass
class PresenceEvent:
    start: datetime
    last: datetime
    samples: int = 0
    motion_samples: int = 0
    max_target_count: int = 0
    source_counts: Counter[Any] = field(default_factory=Counter)
    local_sources: Counter[str] = field(default_factory=Counter)
    coordinates: CoordinateStats = field(default_factory=CoordinateStats)

    def add(
        self,
        captured_at: datetime,
        authority: dict[str, Any],
        devices: dict[str, Any],
    ) -> None:
        self.last = captured_at
        self.samples += 1
        automation = _object(authority.get("automation"))
        if automation.get("motion") is True:
            self.motion_samples += 1
        target_count = _number(automation.get("targetCount")) or 0
        self.max_target_count = max(self.max_target_count, int(target_count))
        for alias, raw in devices.items():
            item = _object(raw)
            if item.get("ok") is not True:
                continue
            local = _object(_object(item.get("data")).get("local"))
            if (_number(local.get("targetCount")) or 0) > 0:
                self.local_sources[alias] += 1
        for raw_target in _array(authority.get("targets")):
            target = _object(raw_target)
            source_count = _number(target.get("sourceCount"))
            if source_count is not None:
                self.source_counts[int(source_count)] += 1
            self.coordinates.add(target.get("x"), target.get("y"))

    def report(
        self,
        state_interval_seconds: float,
        display_timezone: tzinfo,
    ) -> dict[str, Any]:
        return {
            "start": format_timestamp(self.start, display_timezone),
            "end": format_timestamp(self.last, display_timezone),
            "durationSeconds": round(self.samples * state_interval_seconds, 3),
            "sampleCount": self.samples,
            "motionSampleCount": self.motion_samples,
            "maxTargetCount": self.max_target_count,
            "sourceCountDistribution": _counter_dict(self.source_counts),
            "localSourceSamples": _counter_dict(self.local_sources),
            "coordinates": self.coordinates.report(),
        }


@dataclass
class LocalTrackMetrics:
    state_interval_seconds: float
    sample_count: int = 0
    target_sample_count: int = 0
    target_observation_count: int = 0
    multi_target_sample_count: int = 0
    max_target_count: int = 0
    target_counts: Counter[int] = field(default_factory=Counter)
    coordinates: CoordinateStats = field(default_factory=CoordinateStats)
    coordinate_bins: Counter[tuple[int, int]] = field(default_factory=Counter)
    step_distances: list[float] = field(default_factory=list)
    episode_durations: list[float] = field(default_factory=list)
    gap_durations: list[float] = field(default_factory=list)
    active_episode_samples: int = 0
    active_episode_last: datetime | None = None
    previous_episode_end: datetime | None = None
    previous_single_position: tuple[float, float] | None = None
    previous_position_time: datetime | None = None

    def add(self, captured_at: datetime, local: dict[str, Any]) -> None:
        self.sample_count += 1
        target_count = int(_number(local.get("targetCount")) or 0)
        targets = []
        for raw in _array(local.get("targets")):
            target = _object(raw)
            x = _number(target.get("x"))
            y = _number(target.get("y"))
            if x is not None and y is not None:
                targets.append((x, y))
                self.coordinates.add(x, y)
                self.coordinate_bins[
                    (round(x / 250) * 250, round(y / 250) * 250)
                ] += 1
        self.target_counts[target_count] += 1
        self.max_target_count = max(self.max_target_count, target_count)
        self.target_observation_count += len(targets)
        if target_count > 0:
            self.target_sample_count += 1
            self.multi_target_sample_count += target_count > 1
            if self.active_episode_samples == 0:
                if self.previous_episode_end is not None:
                    gap = (
                        captured_at - self.previous_episode_end
                    ).total_seconds() - self.state_interval_seconds
                    self.gap_durations.append(max(0.0, gap))
            elif (
                self.active_episode_last is not None
                and (captured_at - self.active_episode_last).total_seconds()
                > self.state_interval_seconds * 2.5
            ):
                self._finish_episode()
                if self.previous_episode_end is not None:
                    gap = (
                        captured_at - self.previous_episode_end
                    ).total_seconds() - self.state_interval_seconds
                    self.gap_durations.append(max(0.0, gap))
            self.active_episode_samples += 1
            self.active_episode_last = captured_at
        else:
            self._finish_episode()

        if len(targets) == 1:
            position = targets[0]
            if (
                self.previous_single_position is not None
                and self.previous_position_time is not None
                and (captured_at - self.previous_position_time).total_seconds()
                <= self.state_interval_seconds * 2.5
            ):
                dx = position[0] - self.previous_single_position[0]
                dy = position[1] - self.previous_single_position[1]
                self.step_distances.append(math.hypot(dx, dy))
            self.previous_single_position = position
            self.previous_position_time = captured_at
        else:
            self.previous_single_position = None
            self.previous_position_time = None

    def _finish_episode(self) -> None:
        if self.active_episode_samples == 0:
            return
        self.episode_durations.append(
            self.active_episode_samples * self.state_interval_seconds
        )
        self.previous_episode_end = self.active_episode_last
        self.active_episode_samples = 0
        self.active_episode_last = None
        self.previous_single_position = None
        self.previous_position_time = None

    def report(self) -> dict[str, Any]:
        self._finish_episode()
        top_bins = [
            {"x": key[0], "y": key[1], "sampleCount": count}
            for key, count in self.coordinate_bins.most_common(20)
        ]
        return {
            "sampleCount": self.sample_count,
            "targetSampleCount": self.target_sample_count,
            "targetSampleRatio": ratio(
                self.target_sample_count,
                self.sample_count,
            ),
            "targetObservationCount": self.target_observation_count,
            "targetCountDistribution": _counter_dict(self.target_counts),
            "multiTargetSampleCount": self.multi_target_sample_count,
            "maxTargetCount": self.max_target_count,
            "episodeCount": len(self.episode_durations),
            "episodeDurationP50Seconds": percentile(
                self.episode_durations,
                0.50,
            ),
            "episodeDurationP95Seconds": percentile(
                self.episode_durations,
                0.95,
            ),
            "episodeDurationMaxSeconds": (
                round(max(self.episode_durations), 3)
                if self.episode_durations else None
            ),
            "reappearanceGapCount": len(self.gap_durations),
            "reappearanceGapP50Seconds": percentile(
                self.gap_durations,
                0.50,
            ),
            "reappearanceGapP95Seconds": percentile(
                self.gap_durations,
                0.95,
            ),
            "singleTargetStepSampleCount": len(self.step_distances),
            "singleTargetStepP50Mm": percentile(self.step_distances, 0.50),
            "singleTargetStepP95Mm": percentile(self.step_distances, 0.95),
            "singleTargetStepMaxMm": (
                round(max(self.step_distances), 1)
                if self.step_distances else None
            ),
            "singleTargetStepOver500MmCount": sum(
                distance > 500 for distance in self.step_distances
            ),
            "singleTargetStepOver1000MmCount": sum(
                distance > 1000 for distance in self.step_distances
            ),
            "coordinateStats": self.coordinates.report(),
            "topCoordinateBins250mm": top_bins,
        }


@dataclass
class DeviceComparisonMetrics:
    left_alias: str
    right_alias: str
    sample_count: int = 0
    left_unavailable: int = 0
    right_unavailable: int = 0
    both_detected: int = 0
    left_only: int = 0
    right_only: int = 0
    neither_detected: int = 0
    target_count_matches: int = 0
    target_count_differences: Counter[int] = field(default_factory=Counter)
    both_detected_group_corroborated: int = 0
    authoritative_multi_target_samples: int = 0

    def add(self, devices: dict[str, Any], authority: dict[str, Any]) -> None:
        left = _object(devices.get(self.left_alias))
        right = _object(devices.get(self.right_alias))
        left_ok = left.get("ok") is True
        right_ok = right.get("ok") is True
        self.left_unavailable += not left_ok
        self.right_unavailable += not right_ok
        if not left_ok or not right_ok:
            return
        self.sample_count += 1
        left_local = _object(_object(left.get("data")).get("local"))
        right_local = _object(_object(right.get("data")).get("local"))
        left_count = int(_number(left_local.get("targetCount")) or 0)
        right_count = int(_number(right_local.get("targetCount")) or 0)
        left_detected = left_count > 0
        right_detected = right_count > 0
        if left_detected and right_detected:
            self.both_detected += 1
        elif left_detected:
            self.left_only += 1
        elif right_detected:
            self.right_only += 1
        else:
            self.neither_detected += 1
        self.target_count_matches += left_count == right_count
        self.target_count_differences[left_count - right_count] += 1
        targets = [_object(item) for item in _array(authority.get("targets"))]
        if len(targets) > 1:
            self.authoritative_multi_target_samples += 1
        if left_detected and right_detected and any(
            (_number(target.get("sourceCount")) or 0) >= 2
            for target in targets
        ):
            self.both_detected_group_corroborated += 1

    def report(self) -> dict[str, Any]:
        union = self.both_detected + self.left_only + self.right_only
        left_active = self.both_detected + self.left_only
        right_active = self.both_detected + self.right_only
        return {
            "left": self.left_alias,
            "right": self.right_alias,
            "sampleCount": self.sample_count,
            "leftUnavailableSampleCount": self.left_unavailable,
            "rightUnavailableSampleCount": self.right_unavailable,
            "bothDetectedSampleCount": self.both_detected,
            "leftOnlySampleCount": self.left_only,
            "rightOnlySampleCount": self.right_only,
            "neitherDetectedSampleCount": self.neither_detected,
            "detectionAgreementRatio": ratio(
                self.both_detected + self.neither_detected,
                self.sample_count,
            ),
            "detectionJaccardRatio": ratio(self.both_detected, union),
            "leftCoverageWhenRightDetected": ratio(
                self.both_detected,
                right_active,
            ),
            "rightCoverageWhenLeftDetected": ratio(
                self.both_detected,
                left_active,
            ),
            "targetCountMatchSampleCount": self.target_count_matches,
            "targetCountMatchRatio": ratio(
                self.target_count_matches,
                self.sample_count,
            ),
            "targetCountDifferenceDistribution": _counter_dict(
                self.target_count_differences
            ),
            "bothDetectedGroupCorroboratedSampleCount": (
                self.both_detected_group_corroborated
            ),
            "bothDetectedGroupCorroborationRatio": ratio(
                self.both_detected_group_corroborated,
                self.both_detected,
            ),
            "authoritativeMultiTargetSampleCount": (
                self.authoritative_multi_target_samples
            ),
            "localCoordinateFramesComparable": False,
            "fusedSourceIdentityAvailable": False,
        }


@dataclass
class DeviceStateCounters:
    samples: int = 0
    local_target: int = 0
    local_presence: int = 0
    local_motion: int = 0
    static_presence: int = 0
    static_moving: int = 0
    static_still: int = 0
    assist_active: int = 0
    device_presence: int = 0
    device_motion: int = 0
    automation_presence: int = 0
    local_track: LocalTrackMetrics | None = None

    def add(
        self,
        captured_at: datetime,
        data: dict[str, Any],
        state_interval_seconds: float,
    ) -> None:
        local = _object(data.get("local"))
        static = _object(_object(data.get("debug")).get("staticRadar"))
        automation = _object(data.get("automation"))
        self.samples += 1
        self.local_target += (_number(local.get("targetCount")) or 0) > 0
        self.local_presence += local.get("presence") is True
        self.local_motion += local.get("motion") is True
        self.static_presence += static.get("presence") is True
        self.static_moving += static.get("moving") is True
        self.static_still += static.get("still") is True
        self.assist_active += static.get("assistActive") is True
        self.device_presence += data.get("presence") is True
        self.device_motion += data.get("motion") is True
        self.automation_presence += automation.get("presence") is True
        if self.local_track is None:
            self.local_track = LocalTrackMetrics(state_interval_seconds)
        self.local_track.add(captured_at, local)

    def report(self) -> dict[str, Any]:
        values = {
            "localTarget": self.local_target,
            "localPresence": self.local_presence,
            "localMotion": self.local_motion,
            "staticPresence": self.static_presence,
            "staticMoving": self.static_moving,
            "staticStill": self.static_still,
            "assistActive": self.assist_active,
            "devicePresence": self.device_presence,
            "deviceMotion": self.device_motion,
            "automationPresence": self.automation_presence,
        }
        return {
            "sampleCount": self.samples,
            **{
                f"{name}SampleCount": count
                for name, count in values.items()
            },
            **{
                f"{name}Ratio": ratio(count, self.samples)
                for name, count in values.items()
            },
            "localTrack": (
                self.local_track.report()
                if self.local_track is not None
                else None
            ),
        }


@dataclass
class LabelAccumulator:
    interval: LabelInterval
    state_interval_seconds: float
    display_timezone: tzinfo = timezone.utc
    comparison_pairs: tuple[tuple[str, str], ...] = ()
    sample_count: int = 0
    unavailable_samples: int = 0
    presence_samples: int = 0
    motion_samples: int = 0
    target_samples: int = 0
    max_target_count: int = 0
    target_count_mismatch_samples: int = 0
    first_sample: datetime | None = None
    last_sample: datetime | None = None
    first_presence: datetime | None = None
    source_counts: Counter[Any] = field(default_factory=Counter)
    local_sources: Counter[str] = field(default_factory=Counter)
    coordinate_bins: Counter[tuple[int, int]] = field(default_factory=Counter)
    device_counters: dict[str, DeviceStateCounters] = field(
        default_factory=lambda: defaultdict(DeviceStateCounters)
    )
    events: list[dict[str, Any]] = field(default_factory=list)
    event_count: int = 0
    active_event: PresenceEvent | None = None
    comparison_metrics: dict[tuple[str, str], DeviceComparisonMetrics] = field(
        default_factory=dict
    )

    def __post_init__(self) -> None:
        self.comparison_metrics = {
            pair: DeviceComparisonMetrics(*pair)
            for pair in self.comparison_pairs
        }

    def add_unavailable(self) -> None:
        self.unavailable_samples += 1
        self._finish_event()

    def add(
        self,
        captured_at: datetime,
        authority: dict[str, Any],
        devices: dict[str, Any],
    ) -> None:
        self.sample_count += 1
        self.first_sample = self.first_sample or captured_at
        self.last_sample = captured_at
        automation = _object(authority.get("automation"))
        present = automation.get("presence") is True
        motion = automation.get("motion") is True
        target_count = int(_number(automation.get("targetCount")) or 0)
        self.presence_samples += present
        self.motion_samples += motion
        self.target_samples += target_count > 0
        self.max_target_count = max(self.max_target_count, target_count)
        if (
            self.interval.expected_target_count is not None
            and target_count != self.interval.expected_target_count
        ):
            self.target_count_mismatch_samples += 1
        if present and self.first_presence is None:
            self.first_presence = captured_at
        for alias, raw in devices.items():
            item = _object(raw)
            if item.get("ok") is not True:
                continue
            data = _object(item.get("data"))
            self.device_counters[alias].add(
                captured_at,
                data,
                self.state_interval_seconds,
            )
            local = _object(data.get("local"))
            if (_number(local.get("targetCount")) or 0) > 0:
                self.local_sources[alias] += 1
        for raw_target in _array(authority.get("targets")):
            target = _object(raw_target)
            source_count = _number(target.get("sourceCount"))
            if source_count is not None:
                self.source_counts[int(source_count)] += 1
            x = _number(target.get("x"))
            y = _number(target.get("y"))
            if x is not None and y is not None:
                self.coordinate_bins[(round(x / 500) * 500, round(y / 500) * 500)] += 1
        for comparison in self.comparison_metrics.values():
            comparison.add(devices, authority)
        if present:
            if self.active_event is None:
                self.active_event = PresenceEvent(captured_at, captured_at)
            elif (
                captured_at - self.active_event.last
            ).total_seconds() > self.state_interval_seconds * 2.5:
                self._finish_event()
                self.active_event = PresenceEvent(captured_at, captured_at)
            self.active_event.add(captured_at, authority, devices)
        else:
            self._finish_event()

    def _finish_event(self) -> None:
        if self.active_event is None:
            return
        self.event_count += 1
        if len(self.events) < MAX_EVENT_DETAILS:
            self.events.append(
                self.active_event.report(
                    self.state_interval_seconds,
                    self.display_timezone,
                )
            )
        self.active_event = None

    def report(self) -> dict[str, Any]:
        self._finish_event()
        start = self.interval.start or self.first_sample
        end = self.interval.end or self.last_sample
        latency = None
        if self.first_presence is not None and start is not None:
            latency = max(0.0, (self.first_presence - start).total_seconds())
        bins = [
            {"x": key[0], "y": key[1], "sampleCount": count}
            for key, count in self.coordinate_bins.most_common(20)
        ]
        result: dict[str, Any] = {
            "name": self.interval.name,
            "expectedState": self.interval.expected_state,
            "expectedTargetCount": self.interval.expected_target_count,
            "start": format_timestamp(start, self.display_timezone),
            "end": format_timestamp(end, self.display_timezone),
            "sampleCount": self.sample_count,
            "unavailableSampleCount": self.unavailable_samples,
            "availabilityRatio": ratio(
                self.sample_count,
                self.sample_count + self.unavailable_samples,
            ),
            "presenceSampleCount": self.presence_samples,
            "presenceRatio": ratio(self.presence_samples, self.sample_count),
            "motionSampleCount": self.motion_samples,
            "targetSampleCount": self.target_samples,
            "maxTargetCount": self.max_target_count,
            "targetCountMismatchSampleCount": self.target_count_mismatch_samples,
            "targetCountMismatchRatio": ratio(
                self.target_count_mismatch_samples,
                self.sample_count,
            ),
            "presenceEventCount": self.event_count,
            "presenceSecondsEstimate": round(
                self.presence_samples * self.state_interval_seconds, 3
            ),
            "firstPresenceLatencySeconds": (
                round(latency, 3) if latency is not None else None
            ),
            "sourceCountDistribution": _counter_dict(self.source_counts),
            "localTargetSamplesByDevice": _counter_dict(self.local_sources),
            "topCoordinateBins500mm": bins,
            "devices": {
                alias: counters.report()
                for alias, counters in sorted(self.device_counters.items())
            },
            "comparisons": [
                comparison.report()
                for comparison in self.comparison_metrics.values()
            ],
            "events": self.events,
            "eventDetailsTruncated": self.event_count > len(self.events),
        }
        if self.interval.expected_state == "unoccupied":
            result["falsePresenceSampleCount"] = self.presence_samples
            result["falsePresenceRatio"] = ratio(
                self.presence_samples, self.sample_count
            )
            result["falsePresenceEventCount"] = self.event_count
        elif self.interval.expected_state == "occupied":
            missed = self.sample_count - self.presence_samples
            result["missedPresenceSampleCount"] = missed
            result["missedPresenceRatio"] = ratio(missed, self.sample_count)
        return result


@dataclass
class ResourceTrend:
    first_time: datetime | None = None
    last_time: datetime | None = None
    first_free: float | None = None
    last_free: float | None = None
    min_free: float | None = None
    max_free: float | None = None
    min_largest_block: float | None = None
    min_httpd_stack: float | None = None
    historical_min_free_first: float | None = None
    historical_min_free_last: float | None = None
    historical_min_free_lowest: float | None = None
    first_uptime: float | None = None
    last_uptime: float | None = None
    reboot_count: int = 0
    n: int = 0
    sum_x: float = 0.0
    sum_y: float = 0.0
    sum_xx: float = 0.0
    sum_xy: float = 0.0
    rssi_sum: float = 0.0
    rssi_count: int = 0
    rssi_min: float | None = None
    rssi_max: float | None = None
    firmware_versions: Counter[str] = field(default_factory=Counter)

    def add(self, captured_at: datetime, data: dict[str, Any]) -> None:
        memory = _object(data.get("memory"))
        firmware = _object(data.get("firmware"))
        wifi = _object(data.get("wifi"))
        free = _number(memory.get("internalFreeBytes"))
        largest = _number(memory.get("internalLargestFreeBlockBytes"))
        stack = _number(memory.get("httpdStackMinFreeBytes"))
        historical_min = _number(memory.get("internalMinFreeBytes"))
        uptime = _number(firmware.get("uptimeSeconds"))
        rssi = _number(wifi.get("rssi"))
        version = firmware.get("version")
        if isinstance(version, str):
            self.firmware_versions[version] += 1
        if uptime is not None:
            if self.last_uptime is not None and uptime < self.last_uptime:
                self.reboot_count += 1
            if self.first_uptime is None:
                self.first_uptime = uptime
            self.last_uptime = uptime
        if largest is not None:
            self.min_largest_block = (
                largest
                if self.min_largest_block is None
                else min(self.min_largest_block, largest)
            )
        if stack is not None:
            self.min_httpd_stack = (
                stack
                if self.min_httpd_stack is None
                else min(self.min_httpd_stack, stack)
            )
        if historical_min is not None:
            if self.historical_min_free_first is None:
                self.historical_min_free_first = historical_min
            self.historical_min_free_last = historical_min
            self.historical_min_free_lowest = (
                historical_min
                if self.historical_min_free_lowest is None
                else min(self.historical_min_free_lowest, historical_min)
            )
        if rssi is not None:
            self.rssi_sum += rssi
            self.rssi_count += 1
            self.rssi_min = rssi if self.rssi_min is None else min(self.rssi_min, rssi)
            self.rssi_max = rssi if self.rssi_max is None else max(self.rssi_max, rssi)
        if free is None:
            return
        if self.first_time is None:
            self.first_time = captured_at
            self.first_free = free
        self.last_time = captured_at
        self.last_free = free
        self.min_free = free if self.min_free is None else min(self.min_free, free)
        self.max_free = free if self.max_free is None else max(self.max_free, free)
        x = (captured_at - self.first_time).total_seconds() / 3600.0
        self.n += 1
        self.sum_x += x
        self.sum_y += free
        self.sum_xx += x * x
        self.sum_xy += x * free

    def report(self) -> dict[str, Any]:
        denominator = self.n * self.sum_xx - self.sum_x * self.sum_x
        slope = None
        if self.n >= 2 and denominator != 0:
            slope = (
                self.n * self.sum_xy - self.sum_x * self.sum_y
            ) / denominator
        return {
            "sampleCount": self.n,
            "internalFreeFirstBytes": self.first_free,
            "internalFreeLastBytes": self.last_free,
            "internalFreeMinBytes": self.min_free,
            "internalFreeMaxBytes": self.max_free,
            "internalFreeTrendBytesPerHour": (
                round(slope, 1) if slope is not None else None
            ),
            "internalHistoricalMinFreeFirstBytes": self.historical_min_free_first,
            "internalHistoricalMinFreeLastBytes": self.historical_min_free_last,
            "internalHistoricalMinFreeLowestBytes": self.historical_min_free_lowest,
            "internalLargestBlockMinBytes": self.min_largest_block,
            "httpdStackMinFreeBytes": self.min_httpd_stack,
            "uptimeDeltaSeconds": (
                self.last_uptime - self.first_uptime
                if self.last_uptime is not None and self.first_uptime is not None
                else None
            ),
            "rebootCount": self.reboot_count,
            "wifiRssiMin": self.rssi_min,
            "wifiRssiMax": self.rssi_max,
            "wifiRssiAverage": (
                round(self.rssi_sum / self.rssi_count, 1)
                if self.rssi_count else None
            ),
            "firmwareVersions": _counter_dict(self.firmware_versions),
        }


class CaptureAnalyzer:
    def __init__(
        self,
        labels: list[LabelInterval],
        display_timezone: tzinfo = timezone.utc,
        comparison_pairs: list[tuple[str, str]] | None = None,
    ) -> None:
        self.labels = labels
        self.display_timezone = display_timezone
        self.comparison_pairs = tuple(comparison_pairs or [])
        self.state_interval_seconds = DEFAULT_STATE_INTERVAL_SECONDS
        self.label_metrics: list[LabelAccumulator] = []
        self.overall: LabelAccumulator | None = None
        self.capture_start: datetime | None = None
        self.capture_end: datetime | None = None
        self.session: dict[str, Any] = {}
        self.summary: dict[str, Any] = {}
        self.record_counts: Counter[str] = Counter()
        self.request_errors: Counter[str] = Counter()
        self.latencies: dict[str, dict[str, list[float]]] = defaultdict(
            lambda: defaultdict(list)
        )
        self.node_aliases: dict[str, str] = {}
        self.authority_aliases: Counter[str] = Counter()
        self.authority_missing = 0
        self.authority_inactive = 0
        self.endpoint_disagreements = 0
        self.disagreement_examples: list[dict[str, Any]] = []
        self.group_anomalies = 0
        self.group_device_samples = 0
        self.group_anomaly_examples: list[dict[str, Any]] = []
        self.site_anomalies = 0
        self.site_device_samples = 0
        self.site_anomaly_examples: list[dict[str, Any]] = []
        self.group_leaders: Counter[str] = Counter()
        self.group_terms: Counter[Any] = Counter()
        self.site_leaders: Counter[str] = Counter()
        self.site_terms: Counter[Any] = Counter()
        self.configuration_owners: Counter[str] = Counter()
        self.configuration_epochs: Counter[Any] = Counter()
        self.configuration_generations: Counter[Any] = Counter()
        self.role_transitions: list[dict[str, Any]] = []
        self.last_role_observations: dict[str, tuple[Any, ...]] = {}
        self.valid_inputs: Counter[Any] = Counter()
        self.local_leader_production_failures = 0
        self.resources: dict[str, ResourceTrend] = defaultdict(ResourceTrend)

    def analyze(self, paths: list[Path]) -> dict[str, Any]:
        for path, line_number, record in read_records(paths):
            self._consume(path, line_number, record)
        if self.capture_start is None or self.capture_end is None:
            raise ValueError("capture contains no timestamped samples")
        if self.overall is None:
            raise ValueError("capture contains no session or state records")
        return self._report(paths)

    def _consume(self, path: Path, line_number: int, record: dict[str, Any]) -> None:
        record_type = record.get("type")
        if record_type == "session":
            if self.session:
                raise ValueError(f"duplicate session record at {path}:{line_number}")
            self.session = record
            available_aliases = set(_object(record.get("devices")))
            for pair in self.comparison_pairs:
                missing = [alias for alias in pair if alias not in available_aliases]
                if missing:
                    raise ValueError(
                        "comparison aliases are not present in the capture: "
                        + ", ".join(missing)
                    )
            intervals = _object(record.get("intervals"))
            state_interval = _number(intervals.get("stateSeconds"))
            if state_interval is not None and state_interval > 0:
                self.state_interval_seconds = state_interval
            self.overall = LabelAccumulator(
                LabelInterval("overall", "unknown"),
                self.state_interval_seconds,
                self.display_timezone,
                self.comparison_pairs,
            )
            self.label_metrics = [
                LabelAccumulator(
                    label,
                    self.state_interval_seconds,
                    self.display_timezone,
                    self.comparison_pairs,
                )
                for label in self.labels
            ]
            self.record_counts[record_type] += 1
            return
        if record_type == "summary":
            self.summary = record
            self.record_counts[record_type] += 1
            return
        if record_type not in ("state", "groups", "configuration", "system"):
            return
        if self.overall is None:
            self.overall = LabelAccumulator(
                LabelInterval("overall", "unknown"),
                self.state_interval_seconds,
                self.display_timezone,
                self.comparison_pairs,
            )
            self.label_metrics = [
                LabelAccumulator(
                    label,
                    self.state_interval_seconds,
                    self.display_timezone,
                    self.comparison_pairs,
                )
                for label in self.labels
            ]
        try:
            captured_at = parse_timestamp(record["capturedAt"])
        except (KeyError, ValueError) as error:
            raise ValueError(
                f"invalid capturedAt at {path}:{line_number}: {error}"
            ) from error
        self.capture_start = (
            min(self.capture_start, captured_at)
            if self.capture_start
            else captured_at
        )
        self.capture_end = (
            max(self.capture_end, captured_at)
            if self.capture_end
            else captured_at
        )
        self.record_counts[record_type] += 1
        devices = _object(record.get("devices"))
        self._collect_requests(record_type, devices)
        if record_type == "state":
            self._consume_state(captured_at, devices)
        elif record_type == "groups":
            self._consume_groups(captured_at, devices)
        elif record_type == "configuration":
            self._consume_configuration(captured_at, devices)
        else:
            self._consume_system(captured_at, devices)

    def _collect_requests(self, record_type: str, devices: dict[str, Any]) -> None:
        for alias, raw in devices.items():
            item = _object(raw)
            if item.get("ok") is not True:
                self.request_errors[record_type] += 1
                continue
            latency = _number(item.get("latencyMs"))
            if latency is not None:
                self.latencies[record_type][alias].append(latency)

    def _select_authority(
        self,
        devices: dict[str, Any],
    ) -> tuple[str, dict[str, Any]] | None:
        successful = {
            alias: _object(_object(raw).get("data"))
            for alias, raw in devices.items()
            if _object(raw).get("ok") is True
        }
        local = [
            (alias, data)
            for alias, data in successful.items()
            if _object(data.get("fusion")).get("source") == "local_group_leader"
            and _object(data.get("fusion")).get("authoritative") is True
        ]
        if len(local) == 1:
            return local[0]
        leader_ids = Counter(
            _object(data.get("automation")).get("leaderNodeId")
            for data in successful.values()
            if isinstance(_object(data.get("automation")).get("leaderNodeId"), str)
        )
        if leader_ids:
            leader_id = leader_ids.most_common(1)[0][0]
            alias = self.node_aliases.get(leader_id)
            if alias in successful:
                return alias, successful[alias]
        authoritative = [
            (alias, data)
            for alias, data in successful.items()
            if _object(data.get("fusion")).get("authoritative") is True
        ]
        if not authoritative:
            return None
        signatures = Counter(
            self._automation_signature(data)
            for _, data in authoritative
        )
        majority = signatures.most_common(1)[0][0]
        return next(
            (alias, data)
            for alias, data in authoritative
            if self._automation_signature(data) == majority
        )

    @staticmethod
    def _automation_signature(data: dict[str, Any]) -> tuple[Any, ...]:
        automation = _object(data.get("automation"))
        return (
            automation.get("presence"),
            automation.get("motion"),
            automation.get("targetCount"),
            automation.get("leaderNodeId"),
            automation.get("groupTerm"),
        )

    def _consume_state(
        self,
        captured_at: datetime,
        devices: dict[str, Any],
    ) -> None:
        signatures = {
            alias: self._automation_signature(
                _object(_object(raw).get("data"))
            )
            for alias, raw in devices.items()
            if _object(raw).get("ok") is True
        }
        if signatures and len(set(signatures.values())) > 1:
            self.endpoint_disagreements += 1
            if len(self.disagreement_examples) < MAX_ANOMALY_EXAMPLES:
                self.disagreement_examples.append({
                    "capturedAt": format_timestamp(
                        captured_at,
                        self.display_timezone,
                    ),
                    "values": {
                        alias: list(value)
                        for alias, value in signatures.items()
                    },
                })
        selected = self._select_authority(devices)
        metrics = [self.overall] + [
            item for item in self.label_metrics
            if item.interval.contains(captured_at)
        ]
        assert self.overall is not None
        if selected is None:
            self.authority_missing += 1
            for item in metrics:
                assert item is not None
                item.add_unavailable()
            return
        alias, authority = selected
        self.authority_aliases[alias] += 1
        fusion = _object(authority.get("fusion"))
        if fusion.get("authoritative") is not True or fusion.get("state") != "active":
            self.authority_inactive += 1
        for item in metrics:
            assert item is not None
            item.add(captured_at, authority, devices)

    def _consume_groups(self, captured_at: datetime, devices: dict[str, Any]) -> None:
        site_observations: Counter[tuple[Any, ...]] = Counter()
        group_observations: Counter[tuple[Any, ...]] = Counter()
        for alias, raw in devices.items():
            item = _object(raw)
            if item.get("ok") is not True:
                continue
            data = _object(item.get("data"))
            production = _object(data.get("productionFusion"))
            site = _object(data.get("site"))
            site_leader = site.get("leaderNodeId")
            site_term = _number(site.get("term"))
            self.site_device_samples += 1
            site_expected = _number(site.get("participantsExpected"))
            site_online = _number(site.get("participantsOnline"))
            site_anomalous = (
                site.get("health") not in ("healthy", "standalone")
                or not isinstance(site_leader, str)
                or not site_leader
                or site.get("quorumAvailable") is False
                or site_expected is None
                or site_online is None
                or site_expected <= 0
            )
            if site_anomalous:
                self.site_anomalies += 1
                if len(self.site_anomaly_examples) < MAX_ANOMALY_EXAMPLES:
                    self.site_anomaly_examples.append({
                        "capturedAt": format_timestamp(
                            captured_at,
                            self.display_timezone,
                        ),
                        "device": alias,
                        "leaderNodeId": site_leader,
                        "participantsOnline": site_online,
                        "participantsExpected": site_expected,
                        "quorumAvailable": site.get("quorumAvailable"),
                        "health": site.get("health"),
                    })
            if isinstance(site_leader, str) and site_leader:
                self.site_leaders[site_leader] += 1
                site_observations[(site_leader, int(site_term or 0))] += 1
            if site_term is not None:
                self.site_terms[int(site_term)] += 1
            for raw_group in _array(data.get("groups")):
                self.group_device_samples += 1
                group = _object(raw_group)
                leader = group.get("leaderNodeId")
                term = group.get("term")
                if isinstance(leader, str):
                    self.group_leaders[leader] += 1
                    group_observations[(leader, int(term or 0))] += 1
                if _number(term) is not None:
                    self.group_terms[int(term)] += 1
                expected = _number(group.get("participantsExpected"))
                online = _number(group.get("participantsOnline"))
                anomalous = (
                    group.get("health") != "healthy"
                    or not isinstance(leader, str)
                    or not leader
                    or group.get("quorumAvailable") is False
                    or group.get("canonicalAssignment") is False
                    or group.get("credentialsComplete") is False
                    or expected is None
                    or online is None
                    or expected <= 0
                    or online != expected
                )
                if anomalous:
                    self.group_anomalies += 1
                    if len(self.group_anomaly_examples) < MAX_ANOMALY_EXAMPLES:
                        self.group_anomaly_examples.append({
                            "capturedAt": format_timestamp(
                                captured_at,
                                self.display_timezone,
                            ),
                            "device": alias,
                            "groupId": group.get("groupId"),
                            "participantsOnline": online,
                            "participantsExpected": expected,
                            "quorumAvailable": group.get("quorumAvailable"),
                            "canonicalAssignment": group.get(
                                "canonicalAssignment"
                            ),
                            "credentialsComplete": group.get(
                                "credentialsComplete"
                            ),
                            "health": group.get("health"),
                        })
                if group.get("localLeader") is True:
                    shadow = _object(data.get("shadowFusion"))
                    valid_inputs = _number(shadow.get("validInputs"))
                    if valid_inputs is not None:
                        self.valid_inputs[int(valid_inputs)] += 1
                    if (
                        production.get("authoritative") is not True
                        or production.get("state") != "active"
                    ):
                        self.local_leader_production_failures += 1

        self._record_role_transition(
            "siteLeader", captured_at, site_observations
        )
        self._record_role_transition(
            "groupLeader", captured_at, group_observations
        )

    def _consume_configuration(
        self, captured_at: datetime, devices: dict[str, Any]
    ) -> None:
        observations: Counter[tuple[Any, ...]] = Counter()
        for raw in devices.values():
            item = _object(raw)
            if item.get("ok") is not True:
                continue
            data = _object(item.get("data"))
            owner = data.get("configurationOwnerNodeId")
            epoch = _number(data.get("authorityEpoch"))
            generation = _number(data.get("generation"))
            if isinstance(owner, str) and owner:
                self.configuration_owners[owner] += 1
                observations[(owner, int(epoch or 0), int(generation or 0))] += 1
            if epoch is not None:
                self.configuration_epochs[int(epoch)] += 1
            if generation is not None:
                self.configuration_generations[int(generation)] += 1
        self._record_role_transition(
            "configurationOwner", captured_at, observations
        )

    def _record_role_transition(
        self,
        role: str,
        captured_at: datetime,
        observations: Counter[tuple[Any, ...]],
    ) -> None:
        if not observations:
            return
        selected = observations.most_common(1)[0][0]
        if self.last_role_observations.get(role) == selected:
            return
        self.last_role_observations[role] = selected
        transition = {
            "capturedAt": format_timestamp(
                captured_at, self.display_timezone
            ),
            "role": role,
            "nodeId": selected[0],
        }
        if role == "configurationOwner":
            transition["authorityEpoch"] = selected[1]
            transition["generation"] = selected[2]
        else:
            transition["term"] = selected[1]
        self.role_transitions.append(transition)

    def _consume_system(self, captured_at: datetime, devices: dict[str, Any]) -> None:
        for alias, raw in devices.items():
            item = _object(raw)
            if item.get("ok") is not True:
                continue
            data = _object(item.get("data"))
            node_id = _object(data.get("platform")).get("nodeId")
            if isinstance(node_id, str) and node_id:
                self.node_aliases[node_id] = alias
            self.resources[alias].add(captured_at, data)

    def _report(self, paths: list[Path]) -> dict[str, Any]:
        assert self.capture_start is not None
        assert self.capture_end is not None
        assert self.overall is not None
        latency_report = {
            endpoint: {
                alias: {
                    "sampleCount": len(values),
                    "p50Ms": percentile(values, 0.50),
                    "p95Ms": percentile(values, 0.95),
                    "maxMs": round(max(values), 3) if values else None,
                }
                for alias, values in sorted(per_device.items())
            }
            for endpoint, per_device in sorted(self.latencies.items())
        }
        return {
            "schemaVersion": REPORT_SCHEMA_VERSION,
            "generatedAt": datetime.now(timezone.utc).isoformat(
                timespec="milliseconds"
            ),
            "sourceFiles": [str(path) for path in paths],
            "capture": {
                "start": format_timestamp(
                    self.capture_start,
                    self.display_timezone,
                ),
                "end": format_timestamp(
                    self.capture_end,
                    self.display_timezone,
                ),
                "displayTimezoneOffset": format_timezone_offset(
                    self.capture_start,
                    self.display_timezone,
                ),
                "durationSeconds": round(
                    (self.capture_end - self.capture_start).total_seconds(),
                    3,
                ),
                "recordCounts": _counter_dict(self.record_counts),
                "requestErrors": _counter_dict(self.request_errors),
                "recordedSummary": self.summary or None,
            },
            "fusion": {
                "authorityAliases": _counter_dict(self.authority_aliases),
                "authorityMissingSampleCount": self.authority_missing,
                "authorityInactiveSampleCount": self.authority_inactive,
                "endpointDisagreementSampleCount": self.endpoint_disagreements,
                "endpointDisagreementExamples": self.disagreement_examples,
            },
            "group": {
                "leaders": _counter_dict(self.group_leaders),
                "terms": _counter_dict(self.group_terms),
                "validInputs": _counter_dict(self.valid_inputs),
                "deviceSampleCount": self.group_device_samples,
                "anomalyCount": self.group_anomalies,
                "anomalyRatio": ratio(
                    self.group_anomalies,
                    self.group_device_samples,
                ),
                "anomalyExamples": self.group_anomaly_examples,
                "localLeaderProductionFailureCount": (
                    self.local_leader_production_failures
                ),
            },
            "site": {
                "deviceSampleCount": self.site_device_samples,
                "anomalyCount": self.site_anomalies,
                "anomalyRatio": ratio(
                    self.site_anomalies,
                    self.site_device_samples,
                ),
                "anomalyExamples": self.site_anomaly_examples,
            },
            "roles": {
                "configurationOwners": _counter_dict(
                    self.configuration_owners
                ),
                "configurationEpochs": _counter_dict(
                    self.configuration_epochs
                ),
                "configurationGenerations": _counter_dict(
                    self.configuration_generations
                ),
                "siteLeaders": _counter_dict(self.site_leaders),
                "siteTerms": _counter_dict(self.site_terms),
                "groupLeaders": _counter_dict(self.group_leaders),
                "groupTerms": _counter_dict(self.group_terms),
                "transitions": self.role_transitions,
            },
            "latency": latency_report,
            "resources": {
                alias: trend.report()
                for alias, trend in sorted(self.resources.items())
            },
            "overall": self.overall.report(),
            "labels": [item.report() for item in self.label_metrics],
        }


def print_summary(report: dict[str, Any]) -> None:
    capture = report["capture"]
    fusion = report["fusion"]
    group = report["group"]
    site = report["site"]
    roles = report["roles"]
    print(
        "office_noise_analyze: "
        f"duration={capture['durationSeconds'] / 3600:.2f}h "
        f"state_samples={capture['recordCounts'].get('state', 0)} "
        f"request_errors={sum(capture['requestErrors'].values())}"
    )
    print(
        "  fusion: "
        f"authority_missing={fusion['authorityMissingSampleCount']} "
        f"authority_inactive={fusion['authorityInactiveSampleCount']} "
        f"endpoint_disagreements={fusion['endpointDisagreementSampleCount']}"
    )
    print(
        "  site: "
        f"anomalies={site['anomalyCount']}"
    )
    print(
        "  group: "
        f"anomalies={group['anomalyCount']} "
        f"leader_production_failures={group['localLeaderProductionFailureCount']} "
        f"valid_inputs={group['validInputs']}"
    )
    print(
        "  roles: "
        f"owners={roles['configurationOwners']} "
        f"site_leaders={roles['siteLeaders']} "
        f"group_leaders={roles['groupLeaders']} "
        f"transitions={len(roles['transitions'])}"
    )
    for comparison in report["overall"]["comparisons"]:
        print(
            f"  compare {comparison['left']} vs {comparison['right']}: "
            f"samples={comparison['sampleCount']} "
            f"both={comparison['bothDetectedSampleCount']} "
            f"left_only={comparison['leftOnlySampleCount']} "
            f"right_only={comparison['rightOnlySampleCount']} "
            f"jaccard={comparison['detectionJaccardRatio']}"
        )
    for label in report["labels"]:
        line = (
            f"  label {label['name']}: expected={label['expectedState']} "
            f"samples={label['sampleCount']} "
            f"presence={label['presenceRatio']} "
            f"events={label['presenceEventCount']}"
        )
        if label["expectedState"] == "unoccupied":
            line += f" false_presence={label['falsePresenceRatio']}"
        elif label["expectedState"] == "occupied":
            line += f" missed_presence={label['missedPresenceRatio']}"
        print(line)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Analyze one rotated office-noise capture session without loading "
            "the JSONL files into memory."
        ),
        epilog=(
            'Labels JSON: {"schemaVersion":1,"intervals":['
            '{"name":"walking","expectedState":"occupied",'
            '"start":"2026-08-11T09:00:00+08:00",'
            '"end":"2026-08-11T09:05:00+08:00"}]}'
        ),
    )
    parser.add_argument(
        "inputs",
        nargs="+",
        help="Capture JSONL files, a session directory, or one glob pattern.",
    )
    parser.add_argument(
        "--labels",
        type=Path,
        help="Optional schemaVersion 1 JSON file containing labeled intervals.",
    )
    parser.add_argument(
        "--unoccupied-after",
        help="Convenience UTC-offset timestamp for one unoccupied interval.",
    )
    parser.add_argument(
        "--compare",
        action="append",
        default=[],
        help="Compare local tracking for two captured aliases, for example 178,78.",
    )
    parser.add_argument("--output", type=Path, help="Write the JSON report here.")
    args = parser.parse_args()

    try:
        paths = resolve_input_files(args.inputs)
        labels = load_labels(args.labels) if args.labels else []
        if args.unoccupied_after:
            labels.append(LabelInterval(
                "unoccupied-after",
                "unoccupied",
                parse_timestamp(args.unoccupied_after),
                None,
                0,
            ))
        comparisons = [parse_comparison(value) for value in args.compare]
        if len(set(frozenset(pair) for pair in comparisons)) != len(comparisons):
            raise ValueError("duplicate comparison pair")
        local_timezone = datetime.now().astimezone().tzinfo or timezone.utc
        analyzer = CaptureAnalyzer(labels, local_timezone, comparisons)
        report = analyzer.analyze(paths)
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(
                json.dumps(report, ensure_ascii=True, indent=2) + "\n",
                encoding="utf-8",
            )
    except (OSError, ValueError) as error:
        print(f"office_noise_analyze: error: {error}", file=sys.stderr)
        return 2

    print_summary(report)
    if args.output:
        print(f"office_noise_analyze: report={args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

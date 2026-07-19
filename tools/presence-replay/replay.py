#!/usr/bin/env python3
"""Analyze Presence Sensor replay NDJSON logs.

This first replay tool intentionally does not reimplement firmware decisions.
It decodes the compact firmware replay format and scores the outputs that the
device already produced.
"""

from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable


PRESENCE_REASON = {
    0: "none",
    1: "target",
    2: "still_hold",
    3: "pir",
    4: "tracker_assist",
    5: "filter_blocked",
    6: "target_lost_hold_expired",
    7: "tracker_primary",
    8: "tracker_lost_hold_expired",
    9: "static_radar_assist",
    10: "static_radar_clear",
    11: "static_radar_unavailable",
    12: "lost_after_exit",
    13: "lost_without_exit",
    14: "lost_after_room_exit",
    255: "unknown",
}

MOTION_REASON = {
    0: "none",
    1: "moving_target",
    2: "pir",
    3: "motion_hold_expired",
    4: "filter_blocked",
    5: "tracker_motion",
    255: "unknown",
}

STILL_STATE = {
    0: "idle",
    1: "candidate",
    2: "confirmed",
    3: "moving_decay",
    4: "lost",
    255: "unknown",
}

STILL_REASON = {
    0: "none",
    1: "target_missing",
    2: "moving_target",
    3: "anchor_set",
    4: "same_area_still",
    5: "confidence_expired",
    6: "far_from_anchor",
    255: "unknown",
}

RANGE_REASON = {
    0: "ok",
    1: "invalid_distance",
    2: "out_of_range",
    3: "remote_before_pir",
    4: "remote_pir_validated",
    5: "remote_without_pir",
    255: "unknown",
}

TRACKER_STATE = {
    0: "idle",
    1: "tentative",
    2: "confirmed",
    3: "coasting",
    255: "unknown",
}

TRACKER_REASON = {
    0: "none",
    1: "confirmed_by_hits",
    2: "confirmed_by_pir_hint",
    3: "confirmed_reacquired",
    4: "coasting_missed",
    5: "pir",
    6: "idle",
    7: "filter_blocked",
    8: "filter_blocked_missed",
    9: "tentative_new",
    10: "tentative_waiting_hits",
    11: "tentative_missed",
    12: "coasting_after_exit",
    13: "lost_after_exit",
    14: "lost_without_exit",
    15: "coasting_after_room_exit",
    16: "lost_after_room_exit",
    255: "unknown",
}


@dataclass(frozen=True)
class Target:
    valid: bool
    x_mm: int
    y_mm: int
    speed_cm_s: int
    distance_mm: int


@dataclass(frozen=True)
class RawTarget(Target):
    filter_mode: int
    filtered: bool
    range_valid: bool


@dataclass(frozen=True)
class ReplaySample:
    sequence: int
    ms: int
    pir_motion: bool
    illuminance_lux: float
    raw_targets: tuple[RawTarget, RawTarget, RawTarget]
    targets: tuple[Target, Target, Target]
    static_radar_available: bool
    static_radar_presence: bool
    static_radar_moving: bool
    static_radar_still: bool
    static_radar_moving_distance_mm: int
    static_radar_still_distance_mm: int
    static_radar_moving_energy: int
    static_radar_still_energy: int
    static_radar_detection_distance_mm: int
    pir_evidence: bool
    tracker_evidence: bool
    static_assist_armed: bool
    static_assist_active: bool
    static_assist_arm_pending: bool
    static_assist_exit_veto: bool
    static_assist_arm_elapsed_ms: int
    filter_blocked: bool
    range_reason_code: int
    range_suspect_count: int
    range_out_of_range_count: int
    range_remote_candidate_count: int
    exit_active: bool
    exit_zone_mask: int
    exit_target_count: int
    exit_last_seen_age_ms: int
    presence: bool
    motion: bool
    still: bool
    target_count: int
    moving_count: int
    still_count: int
    still_confidence: int
    empty_samples: int
    presence_reason_code: int
    presence_off_reason_code: int
    motion_reason_code: int
    still_state_code: int
    still_reason_code: int
    tracker_presence: bool
    tracker_motion: bool
    tracker_state_code: int
    tracker_reason_code: int
    tracker_score: int
    tracker_input_count: int
    tracker_active_count: int
    tracker_tentative_count: int
    tracker_confirmed_count: int
    tracker_coasting_count: int
    tracker_moving_count: int
    tracker_still_count: int

    @property
    def raw_present(self) -> bool:
        return any(target.valid for target in self.raw_targets)

    @property
    def post_present(self) -> bool:
        return any(target.valid for target in self.targets)

    @property
    def range_reason(self) -> str:
        return code_name(RANGE_REASON, self.range_reason_code)

    @property
    def presence_reason(self) -> str:
        return code_name(PRESENCE_REASON, self.presence_reason_code)

    @property
    def presence_off_reason(self) -> str:
        return code_name(PRESENCE_REASON, self.presence_off_reason_code)

    @property
    def motion_reason(self) -> str:
        return code_name(MOTION_REASON, self.motion_reason_code)

    @property
    def still_state(self) -> str:
        return code_name(STILL_STATE, self.still_state_code)

    @property
    def still_reason(self) -> str:
        return code_name(STILL_REASON, self.still_reason_code)

    @property
    def tracker_state(self) -> str:
        return code_name(TRACKER_STATE, self.tracker_state_code)

    @property
    def tracker_reason(self) -> str:
        return code_name(TRACKER_REASON, self.tracker_reason_code)


@dataclass(frozen=True)
class Segment:
    start_ms: int
    end_ms: int
    samples: tuple[ReplaySample, ...]

    @property
    def duration_ms(self) -> int:
        return max(0, self.end_ms - self.start_ms)


@dataclass(frozen=True)
class TruthSegment:
    label: str
    start_ms: int
    end_ms: int
    occupied: bool
    note: str = ""

    @property
    def duration_ms(self) -> int:
        return max(0, self.end_ms - self.start_ms)


@dataclass(frozen=True)
class NativeTrackerSample:
    sequence: int
    ms: int
    presence: bool
    motion: bool
    state_code: int
    reason_code: int
    score: int
    input_count: int
    active_count: int
    tentative_count: int
    confirmed_count: int
    coasting_count: int
    moving_count: int
    still_count: int

    @property
    def state(self) -> str:
        return code_name(TRACKER_STATE, self.state_code)

    @property
    def reason(self) -> str:
        return code_name(TRACKER_REASON, self.reason_code)


def code_name(mapping: dict[int, str], code: int) -> str:
    return mapping.get(code, f"code_{code}")


def parse_bool_int(value: object) -> bool:
    return bool(int(value))


def parse_raw_target(value: list[object]) -> RawTarget:
    if len(value) != 8:
        raise ValueError(f"raw target tuple must have 8 items, got {len(value)}")
    return RawTarget(
        valid=parse_bool_int(value[0]),
        x_mm=int(value[1]),
        y_mm=int(value[2]),
        speed_cm_s=int(value[3]),
        distance_mm=int(value[4]),
        filter_mode=int(value[5]),
        filtered=parse_bool_int(value[6]),
        range_valid=parse_bool_int(value[7]),
    )


def parse_target(value: list[object]) -> Target:
    if len(value) != 5:
        raise ValueError(f"target tuple must have 5 items, got {len(value)}")
    return Target(
        valid=parse_bool_int(value[0]),
        x_mm=int(value[1]),
        y_mm=int(value[2]),
        speed_cm_s=int(value[3]),
        distance_mm=int(value[4]),
    )


def parse_sample(row: dict[str, object], line_number: int) -> ReplaySample:
    try:
        raw_targets = tuple(parse_raw_target(value) for value in row["r"])
        targets = tuple(parse_target(value) for value in row["tg"])
        static_radar_tuple = row.get("sr", [0, 0, 0, 0, 0, 0, 0, 0])
        static_fusion_tuple = row.get("sf", [0, 0, 0, 0, 0, 0, 0])
        filter_tuple = row["f"]
        exit_tuple = row.get("ex", [0, 0, 0, -1])
        legacy_tuple = row["l"]
        tracker_tuple = row["tr"]
    except KeyError as exc:
        raise ValueError(f"line {line_number}: missing field {exc}") from exc

    if len(raw_targets) != 3 or len(targets) != 3:
        raise ValueError(f"line {line_number}: expected exactly 3 targets")
    if len(filter_tuple) != 5:
        raise ValueError(f"line {line_number}: filter tuple must have 5 items")
    if len(static_radar_tuple) not in (8, 9):
        raise ValueError(f"line {line_number}: static radar tuple must have 8 or 9 items")
    if len(static_fusion_tuple) != 7:
        raise ValueError(f"line {line_number}: static fusion tuple must have 7 items")
    if len(exit_tuple) != 4:
        raise ValueError(f"line {line_number}: exit tuple must have 4 items")
    if len(legacy_tuple) != 13:
        raise ValueError(f"line {line_number}: legacy tuple must have 13 items")
    if len(tracker_tuple) != 12:
        raise ValueError(f"line {line_number}: tracker tuple must have 12 items")

    return ReplaySample(
        sequence=int(row["q"]),
        ms=int(row["t"]),
        pir_motion=parse_bool_int(row["p"]),
        illuminance_lux=float(row["lx"]),
        raw_targets=raw_targets,  # type: ignore[arg-type]
        targets=targets,  # type: ignore[arg-type]
        static_radar_available=parse_bool_int(static_radar_tuple[0]),
        static_radar_presence=parse_bool_int(static_radar_tuple[1]),
        static_radar_moving=parse_bool_int(static_radar_tuple[2]),
        static_radar_still=parse_bool_int(static_radar_tuple[3]),
        static_radar_moving_distance_mm=int(static_radar_tuple[4]),
        static_radar_still_distance_mm=int(static_radar_tuple[5]),
        static_radar_moving_energy=int(static_radar_tuple[6]),
        static_radar_still_energy=int(static_radar_tuple[7]),
        static_radar_detection_distance_mm=int(static_radar_tuple[8]) if len(static_radar_tuple) == 9 else 0,
        pir_evidence=parse_bool_int(static_fusion_tuple[0]),
        tracker_evidence=parse_bool_int(static_fusion_tuple[1]),
        static_assist_armed=parse_bool_int(static_fusion_tuple[2]),
        static_assist_active=parse_bool_int(static_fusion_tuple[3]),
        static_assist_arm_pending=parse_bool_int(static_fusion_tuple[4]),
        static_assist_exit_veto=parse_bool_int(static_fusion_tuple[5]),
        static_assist_arm_elapsed_ms=int(static_fusion_tuple[6]),
        filter_blocked=parse_bool_int(filter_tuple[0]),
        range_reason_code=int(filter_tuple[1]),
        range_suspect_count=int(filter_tuple[2]),
        range_out_of_range_count=int(filter_tuple[3]),
        range_remote_candidate_count=int(filter_tuple[4]),
        exit_active=parse_bool_int(exit_tuple[0]),
        exit_zone_mask=int(exit_tuple[1]),
        exit_target_count=int(exit_tuple[2]),
        exit_last_seen_age_ms=int(exit_tuple[3]),
        presence=parse_bool_int(legacy_tuple[0]),
        motion=parse_bool_int(legacy_tuple[1]),
        still=parse_bool_int(legacy_tuple[2]),
        target_count=int(legacy_tuple[3]),
        moving_count=int(legacy_tuple[4]),
        still_count=int(legacy_tuple[5]),
        still_confidence=int(legacy_tuple[6]),
        empty_samples=int(legacy_tuple[7]),
        presence_reason_code=int(legacy_tuple[8]),
        presence_off_reason_code=int(legacy_tuple[9]),
        motion_reason_code=int(legacy_tuple[10]),
        still_state_code=int(legacy_tuple[11]),
        still_reason_code=int(legacy_tuple[12]),
        tracker_presence=parse_bool_int(tracker_tuple[0]),
        tracker_motion=parse_bool_int(tracker_tuple[1]),
        tracker_state_code=int(tracker_tuple[2]),
        tracker_reason_code=int(tracker_tuple[3]),
        tracker_score=int(tracker_tuple[4]),
        tracker_input_count=int(tracker_tuple[5]),
        tracker_active_count=int(tracker_tuple[6]),
        tracker_tentative_count=int(tracker_tuple[7]),
        tracker_confirmed_count=int(tracker_tuple[8]),
        tracker_coasting_count=int(tracker_tuple[9]),
        tracker_moving_count=int(tracker_tuple[10]),
        tracker_still_count=int(tracker_tuple[11]),
    )


def load_samples(path: Path) -> list[ReplaySample]:
    samples: list[ReplaySample] = []
    with path.open("r", encoding="utf-8-sig") as file:
        for line_number, line in enumerate(file, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
                samples.append(parse_sample(row, line_number))
            except Exception as exc:
                raise ValueError(f"{path}:{line_number}: {exc}") from exc
    samples.sort(key=lambda sample: (sample.ms, sample.sequence))
    return samples


def parse_native_tracker_sample(row: dict[str, object], line_number: int) -> NativeTrackerSample:
    try:
        tracker_tuple = row["tr"]
    except KeyError as exc:
        raise ValueError(f"native line {line_number}: missing field {exc}") from exc
    if len(tracker_tuple) != 12:
        raise ValueError(f"native line {line_number}: tracker tuple must have 12 items")

    return NativeTrackerSample(
        sequence=int(row["q"]),
        ms=int(row["t"]),
        presence=parse_bool_int(tracker_tuple[0]),
        motion=parse_bool_int(tracker_tuple[1]),
        state_code=int(tracker_tuple[2]),
        reason_code=int(tracker_tuple[3]),
        score=int(tracker_tuple[4]),
        input_count=int(tracker_tuple[5]),
        active_count=int(tracker_tuple[6]),
        tentative_count=int(tracker_tuple[7]),
        confirmed_count=int(tracker_tuple[8]),
        coasting_count=int(tracker_tuple[9]),
        moving_count=int(tracker_tuple[10]),
        still_count=int(tracker_tuple[11]),
    )


def run_native_tracker(runner: Path, log_path: Path) -> dict[int, NativeTrackerSample]:
    completed = subprocess.run(
        [str(runner), str(log_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if completed.returncode != 0:
        raise ValueError(f"native tracker runner failed: {completed.stderr.strip()}")

    outputs: dict[int, NativeTrackerSample] = {}
    for line_number, line in enumerate(completed.stdout.splitlines(), start=1):
        line = line.strip()
        if not line:
            continue
        row = json.loads(line)
        sample = parse_native_tracker_sample(row, line_number)
        outputs[sample.sequence] = sample
    return outputs


def median_interval_ms(samples: list[ReplaySample]) -> int:
    intervals = [
        samples[index + 1].ms - samples[index].ms
        for index in range(len(samples) - 1)
        if samples[index + 1].ms > samples[index].ms
    ]
    if not intervals:
        return 500
    return int(statistics.median(intervals))


def sample_end_ms(samples: list[ReplaySample], index: int, fallback_interval_ms: int) -> int:
    if index + 1 < len(samples) and samples[index + 1].ms > samples[index].ms:
        return samples[index + 1].ms
    return samples[index].ms + fallback_interval_ms


def count_flips(samples: Iterable[ReplaySample], value: Callable[[ReplaySample], object]) -> int:
    sentinel = object()
    previous: object = sentinel
    flips = 0
    for sample in samples:
        current = value(sample)
        if previous is not sentinel and current != previous:
            flips += 1
        previous = current
    return flips


def duration_where(
    samples: list[ReplaySample],
    predicate: Callable[[ReplaySample], bool],
    fallback_interval_ms: int,
) -> int:
    total = 0
    for index, sample in enumerate(samples):
        if predicate(sample):
            total += sample_end_ms(samples, index, fallback_interval_ms) - sample.ms
    return total


def contiguous_segments(
    samples: list[ReplaySample],
    predicate: Callable[[ReplaySample], bool],
    fallback_interval_ms: int,
) -> list[Segment]:
    segments: list[Segment] = []
    current: list[ReplaySample] = []
    start_ms = 0
    end_ms = 0

    for index, sample in enumerate(samples):
        if predicate(sample):
            if not current:
                start_ms = sample.ms
            current.append(sample)
            end_ms = sample_end_ms(samples, index, fallback_interval_ms)
        elif current:
            segments.append(Segment(start_ms=start_ms, end_ms=end_ms, samples=tuple(current)))
            current = []

    if current:
        segments.append(Segment(start_ms=start_ms, end_ms=end_ms, samples=tuple(current)))
    return segments


def parse_hhmmss(value: str) -> int:
    parts = value.split(":")
    if len(parts) not in (2, 3):
        raise ValueError(f"expected HH:MM or HH:MM:SS, got {value!r}")
    hours = int(parts[0])
    minutes = int(parts[1])
    seconds = int(parts[2]) if len(parts) == 3 else 0
    if not (0 <= hours <= 23 and 0 <= minutes <= 59 and 0 <= seconds <= 59):
        raise ValueError(f"invalid time value {value!r}")
    return ((hours * 60 + minutes) * 60 + seconds) * 1000


def parse_duration_or_ms(value: str) -> int:
    text = value.strip().lower()
    multiplier = 1
    if text.endswith("ms"):
        text = text[:-2]
        multiplier = 1
    elif text.endswith("s"):
        text = text[:-1]
        multiplier = 1000
    elif text.endswith("m"):
        text = text[:-1]
        multiplier = 60_000
    return int(float(text) * multiplier)


def resolve_window_ms(
    samples: list[ReplaySample],
    occupied_from: str | None,
    occupied_to: str | None,
    end_time: str | None,
) -> tuple[int, int] | None:
    if occupied_from is None and occupied_to is None:
        return None
    if not samples:
        return None

    first_ms = samples[0].ms
    last_ms = samples[-1].ms

    def resolve(value: str | None, default_ms: int) -> int:
        if value is None:
            return default_ms
        if ":" not in value:
            return first_ms + parse_duration_or_ms(value)
        if end_time is None:
            raise ValueError("--end-time is required when occupied times use HH:MM[:SS]")
        end_clock_ms = parse_hhmmss(end_time)
        sample_clock_ms = parse_hhmmss(value)
        relative_to_end = sample_clock_ms - end_clock_ms
        if relative_to_end > 12 * 60 * 60 * 1000:
            relative_to_end -= 24 * 60 * 60 * 1000
        if relative_to_end < -12 * 60 * 60 * 1000:
            relative_to_end += 24 * 60 * 60 * 1000
        return last_ms + relative_to_end

    start_ms = resolve(occupied_from, first_ms)
    end_ms = resolve(occupied_to, last_ms)
    if end_ms < start_ms:
        raise ValueError("occupied end must be after occupied start")
    return start_ms, end_ms


def resolve_time_value(samples: list[ReplaySample], value: object, end_time: str | None) -> int:
    if not samples:
        raise ValueError("cannot resolve a time value without replay samples")
    if isinstance(value, (int, float)):
        return samples[0].ms + int(float(value) * 1000)
    if not isinstance(value, str):
        raise ValueError(f"time value must be a string or number, got {value!r}")

    text = value.strip()
    if ":" not in text:
        return samples[0].ms + parse_duration_or_ms(text)
    if end_time is None:
        raise ValueError("truth segment wall-clock times require --end-time or top-level endTime")

    end_clock_ms = parse_hhmmss(end_time)
    sample_clock_ms = parse_hhmmss(text)
    relative_to_end = sample_clock_ms - end_clock_ms
    if relative_to_end > 12 * 60 * 60 * 1000:
        relative_to_end -= 24 * 60 * 60 * 1000
    if relative_to_end < -12 * 60 * 60 * 1000:
        relative_to_end += 24 * 60 * 60 * 1000
    return samples[-1].ms + relative_to_end


def load_truth_segments(path: Path, samples: list[ReplaySample], end_time: str | None) -> list[TruthSegment]:
    with path.open("r", encoding="utf-8-sig") as file:
        payload = json.load(file)
    if not isinstance(payload, dict):
        raise ValueError("truth file must contain a JSON object")

    truth_end_time = end_time or payload.get("endTime") or payload.get("replayEndTime")
    segments = payload.get("segments")
    if not isinstance(segments, list):
        raise ValueError("truth file must contain a segments array")

    result: list[TruthSegment] = []
    for index, segment in enumerate(segments, start=1):
        if not isinstance(segment, dict):
            raise ValueError(f"truth segment {index} must be an object")
        if "from" not in segment or "to" not in segment:
            raise ValueError(f"truth segment {index} must contain from and to")

        start_ms = resolve_time_value(samples, segment["from"], truth_end_time)
        end_ms = resolve_time_value(samples, segment["to"], truth_end_time)
        if end_ms < start_ms:
            raise ValueError(f"truth segment {index} ends before it starts")

        label = str(segment.get("label") or segment.get("state") or f"segment_{index}")
        occupied = bool(segment.get("occupied", False))
        note = str(segment.get("note", ""))
        result.append(TruthSegment(label=label, start_ms=start_ms, end_ms=end_ms, occupied=occupied, note=note))
    return result


def filter_window(samples: list[ReplaySample], window: tuple[int, int] | None) -> list[ReplaySample]:
    if window is None:
        return samples
    start_ms, end_ms = window
    return [sample for sample in samples if start_ms <= sample.ms <= end_ms]


def ms_to_seconds(value: int) -> str:
    return f"{value / 1000.0:.1f}s"


def format_uptime(ms: int) -> str:
    seconds = ms // 1000
    millis = ms % 1000
    minutes, second = divmod(seconds, 60)
    hours, minute = divmod(minutes, 60)
    if hours:
        return f"{hours:d}:{minute:02d}:{second:02d}.{millis:03d}"
    return f"{minute:02d}:{second:02d}.{millis:03d}"


def top_counter(counter: Counter[str], limit: int = 6) -> str:
    if not counter:
        return "-"
    return ", ".join(f"{name}={count}" for name, count in counter.most_common(limit))


def segment_reason_counter(segment: Segment, attr: Callable[[ReplaySample], str]) -> Counter[str]:
    return Counter(attr(sample) for sample in segment.samples)


def print_segments(title: str, segments: list[Segment], limit: int) -> None:
    print(f"\n{title}")
    if not segments:
        print("  none")
        return

    print("  start       end         dur     reason/state                  raw post tr  fblk range")
    for segment in segments[:limit]:
        representative = segment.samples[-1]
        reason = segment_reason_counter(segment, lambda sample: sample.presence_off_reason).most_common(1)[0][0]
        tracker_state = segment_reason_counter(segment, lambda sample: sample.tracker_state).most_common(1)[0][0]
        raw_ratio = sum(1 for sample in segment.samples if sample.raw_present)
        post_ratio = sum(1 for sample in segment.samples if sample.post_present)
        tracker_ratio = sum(1 for sample in segment.samples if sample.tracker_presence)
        filter_ratio = sum(1 for sample in segment.samples if sample.filter_blocked)
        print(
            "  "
            f"{format_uptime(segment.start_ms):<11} "
            f"{format_uptime(segment.end_ms):<11} "
            f"{ms_to_seconds(segment.duration_ms):<7} "
            f"{reason + '/' + tracker_state:<29} "
            f"{raw_ratio:>3}/{len(segment.samples):<3} "
            f"{post_ratio:>3}/{len(segment.samples):<3} "
            f"{tracker_ratio:>3}/{len(segment.samples):<3} "
            f"{filter_ratio:>3}/{len(segment.samples):<3} "
            f"{representative.range_reason}"
        )
    if len(segments) > limit:
        print(f"  ... {len(segments) - limit} more")


def segment_samples(samples: list[ReplaySample], truth: TruthSegment) -> list[ReplaySample]:
    return [sample for sample in samples if truth.start_ms <= sample.ms < truth.end_ms]


def ratio_text(numerator_ms: int, denominator_ms: int) -> str:
    if denominator_ms <= 0:
        return "-"
    return f"{numerator_ms / denominator_ms * 100.0:.0f}%"


def print_truth_report(
    samples: list[ReplaySample],
    truth_segments: list[TruthSegment],
    fallback_interval_ms: int,
    native_outputs: dict[int, NativeTrackerSample],
    limit: int,
) -> None:
    print("\nTruth Segment Score")
    if not truth_segments:
        print("  none")
        return

    if native_outputs:
        print("  label           truth  samples dur     legacy_on tracker_on native_on raw    post   issue       flips note")
    else:
        print("  label           truth  samples dur     legacy_on tracker_on raw    post   issue       flips note")
    issue_segments: list[tuple[TruthSegment, list[Segment]]] = []

    for truth in truth_segments:
        selected = segment_samples(samples, truth)
        if not selected:
            native_empty = f"{'-':<9} " if native_outputs else ""
            print(
                "  "
                f"{truth.label[:15]:<15} "
                f"{'occ' if truth.occupied else 'empty':<6} "
                f"{0:>7} "
                f"{ms_to_seconds(truth.duration_ms):<7} "
                f"{'-':<9} {'-':<10} "
                f"{native_empty}"
                f"{'-':<6} {'-':<6} "
                f"{'no_samples':<11} "
                f"{'-':<5} "
                f"{truth.note}"
            )
            continue

        segment_interval_ms = median_interval_ms(selected) if len(selected) > 1 else fallback_interval_ms
        total_ms = duration_where(selected, lambda sample: True, segment_interval_ms)
        legacy_on_ms = duration_where(selected, lambda sample: sample.presence, segment_interval_ms)
        tracker_on_ms = duration_where(selected, lambda sample: sample.tracker_presence, segment_interval_ms)
        native_on_ms = duration_where(
            selected,
            lambda sample: sample.sequence in native_outputs and native_outputs[sample.sequence].presence,
            segment_interval_ms,
        )
        raw_ms = duration_where(selected, lambda sample: sample.raw_present, segment_interval_ms)
        post_ms = duration_where(selected, lambda sample: sample.post_present, segment_interval_ms)
        flips = count_flips(selected, lambda sample: sample.presence)

        if truth.occupied:
            problems = contiguous_segments(selected, lambda sample: not sample.presence, segment_interval_ms)
            issue_ms = sum(segment.duration_ms for segment in problems)
            issue_text = f"off {ms_to_seconds(issue_ms)}"
        else:
            problems = contiguous_segments(selected, lambda sample: sample.presence, segment_interval_ms)
            issue_ms = sum(segment.duration_ms for segment in problems)
            issue_text = f"on {ms_to_seconds(issue_ms)}"

        if problems:
            issue_segments.append((truth, problems))

        print(
            "  "
            f"{truth.label[:15]:<15} "
            f"{'occ' if truth.occupied else 'empty':<6} "
            f"{len(selected):>7} "
            f"{ms_to_seconds(total_ms):<7} "
            f"{ratio_text(legacy_on_ms, total_ms):<9} "
            f"{ratio_text(tracker_on_ms, total_ms):<10} "
            f"{(ratio_text(native_on_ms, total_ms) + ' ') if native_outputs else ''}"
            f"{ratio_text(raw_ms, total_ms):<6} "
            f"{ratio_text(post_ms, total_ms):<6} "
            f"{issue_text:<11} "
            f"{flips:<5} "
            f"{truth.note}"
        )

    for truth, problems in issue_segments[:limit]:
        expected = "occupied" if truth.occupied else "empty"
        title = f"Truth Issue Segments: {truth.label} ({expected})"
        print_segments(title, problems, limit)
    if len(issue_segments) > limit:
        print(f"\n... {len(issue_segments) - limit} more truth segments with issues")


def analyze(
    path: Path,
    samples: list[ReplaySample],
    window: tuple[int, int] | None,
    truth_segments: list[TruthSegment],
    native_outputs: dict[int, NativeTrackerSample],
    limit: int,
) -> None:
    if not samples:
        print(f"{path}: no samples")
        return

    interval_ms = median_interval_ms(samples)
    selected = filter_window(samples, window)
    selected_interval_ms = median_interval_ms(selected) if selected else interval_ms

    print("Presence Replay Summary")
    print(f"  file: {path}")
    print(f"  samples: {len(samples)}")
    print(f"  uptime: {format_uptime(samples[0].ms)} -> {format_uptime(samples[-1].ms)}")
    print(f"  duration: {ms_to_seconds(samples[-1].ms - samples[0].ms)}")
    print(f"  median interval: {interval_ms}ms")

    if window is not None:
        print(f"  occupied window: {format_uptime(window[0])} -> {format_uptime(window[1])}")
        print(f"  selected samples: {len(selected)}")

    if not selected:
        print("\nNo samples in selected window.")
        return

    total_ms = duration_where(selected, lambda sample: True, selected_interval_ms)
    presence_on_ms = duration_where(selected, lambda sample: sample.presence, selected_interval_ms)
    tracker_on_ms = duration_where(selected, lambda sample: sample.tracker_presence, selected_interval_ms)
    native_tracker_on_ms = duration_where(
        selected,
        lambda sample: sample.sequence in native_outputs and native_outputs[sample.sequence].presence,
        selected_interval_ms,
    )
    raw_ms = duration_where(selected, lambda sample: sample.raw_present, selected_interval_ms)
    post_ms = duration_where(selected, lambda sample: sample.post_present, selected_interval_ms)
    filter_ms = duration_where(selected, lambda sample: sample.filter_blocked, selected_interval_ms)
    exit_ms = duration_where(selected, lambda sample: sample.exit_active, selected_interval_ms)
    raw_only_ms = duration_where(
        selected,
        lambda sample: sample.raw_present and not sample.post_present,
        selected_interval_ms,
    )

    print("\nCore Durations")
    print(f"  selected duration: {ms_to_seconds(total_ms)}")
    print(f"  legacy presence on: {ms_to_seconds(presence_on_ms)}")
    print(f"  tracker presence on: {ms_to_seconds(tracker_on_ms)}")
    if native_outputs:
        print(f"  native tracker presence on: {ms_to_seconds(native_tracker_on_ms)}")
    print(f"  raw target present: {ms_to_seconds(raw_ms)}")
    print(f"  post-filter target present: {ms_to_seconds(post_ms)}")
    print(f"  raw target removed by filters/range: {ms_to_seconds(raw_only_ms)}")
    print(f"  filter blocked: {ms_to_seconds(filter_ms)}")
    print(f"  exit zone active: {ms_to_seconds(exit_ms)}")

    print("\nTransitions")
    print(f"  legacy presence flips: {count_flips(selected, lambda sample: sample.presence)}")
    print(f"  legacy motion flips: {count_flips(selected, lambda sample: sample.motion)}")
    print(f"  legacy still flips: {count_flips(selected, lambda sample: sample.still)}")
    print(f"  tracker presence flips: {count_flips(selected, lambda sample: sample.tracker_presence)}")
    print(f"  tracker state flips: {count_flips(selected, lambda sample: sample.tracker_state_code)}")
    print(f"  exit zone active flips: {count_flips(selected, lambda sample: sample.exit_active)}")
    if native_outputs:
        print(
            "  native tracker presence flips: "
            f"{count_flips(selected, lambda sample: native_outputs.get(sample.sequence, NativeTrackerSample(0, 0, False, False, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)).presence)}"
        )

    print("\nReason Counts")
    print(f"  presence on reasons: {top_counter(Counter(sample.presence_reason for sample in selected if sample.presence))}")
    print(f"  presence off reasons: {top_counter(Counter(sample.presence_off_reason for sample in selected if not sample.presence))}")
    print(f"  motion reasons: {top_counter(Counter(sample.motion_reason for sample in selected))}")
    print(f"  still states: {top_counter(Counter(sample.still_state for sample in selected))}")
    print(f"  still reasons: {top_counter(Counter(sample.still_reason for sample in selected))}")
    print(f"  range reasons: {top_counter(Counter(sample.range_reason for sample in selected))}")
    print(f"  tracker states: {top_counter(Counter(sample.tracker_state for sample in selected))}")
    print(f"  tracker reasons: {top_counter(Counter(sample.tracker_reason for sample in selected))}")
    if native_outputs:
        native_selected = [native_outputs[sample.sequence] for sample in selected if sample.sequence in native_outputs]
        print(f"  native tracker states: {top_counter(Counter(sample.state for sample in native_selected))}")
        print(f"  native tracker reasons: {top_counter(Counter(sample.reason for sample in native_selected))}")

    if truth_segments:
        print_truth_report(samples, truth_segments, interval_ms, native_outputs, limit)

    if window is not None:
        false_off = contiguous_segments(selected, lambda sample: not sample.presence, selected_interval_ms)
        print("\nOccupied-Window Score")
        print(f"  false-off count: {len(false_off)}")
        print(f"  false-off seconds: {ms_to_seconds(sum(segment.duration_ms for segment in false_off))}")
        print_segments("False-Off Segments", false_off, limit)

    mismatch = contiguous_segments(
        selected,
        lambda sample: sample.presence != sample.tracker_presence,
        selected_interval_ms,
    )
    native_mismatch = contiguous_segments(
        selected,
        lambda sample: sample.sequence in native_outputs
        and sample.tracker_presence != native_outputs[sample.sequence].presence,
        selected_interval_ms,
    )
    raw_only = contiguous_segments(
        selected,
        lambda sample: sample.raw_present and not sample.post_present,
        selected_interval_ms,
    )
    filter_blocked = contiguous_segments(selected, lambda sample: sample.filter_blocked, selected_interval_ms)
    exit_active = contiguous_segments(selected, lambda sample: sample.exit_active, selected_interval_ms)

    print_segments("Legacy/Tracker Mismatch Segments", mismatch, limit)
    if native_outputs:
        print_segments("Firmware Tracker/Native Tracker Mismatch Segments", native_mismatch, limit)
    print_segments("Raw-Present But Post-Filter-Missing Segments", raw_only, limit)
    print_segments("Filter-Blocked Segments", filter_blocked, limit)
    print_segments("Exit-Zone Active Segments", exit_active, limit)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Analyze presence replay NDJSON logs.")
    parser.add_argument("log", type=Path, help="Path to presence-replay.ndjson")
    parser.add_argument("--truth", type=Path, help="Optional truth JSON with labeled segments.")
    parser.add_argument(
        "--native-runner",
        type=Path,
        help="Optional C++ tracker_runner executable. Runs current PresenceTracker against this replay log.",
    )
    parser.add_argument(
        "--occupied-from",
        help="Occupied window start. Use duration from first sample (e.g. 0, 30s, 2m) or HH:MM[:SS] with --end-time.",
    )
    parser.add_argument(
        "--occupied-to",
        help="Occupied window end. Use duration from first sample (e.g. 10m) or HH:MM[:SS] with --end-time.",
    )
    parser.add_argument(
        "--end-time",
        help="Wall-clock HH:MM[:SS] for the final replay sample. Required when occupied times use HH:MM[:SS].",
    )
    parser.add_argument("--limit", type=int, default=20, help="Maximum segment rows to print per section.")
    return parser


def main(argv: list[str]) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    try:
        samples = load_samples(args.log)
        window = resolve_window_ms(samples, args.occupied_from, args.occupied_to, args.end_time)
        truth_segments = load_truth_segments(args.truth, samples, args.end_time) if args.truth else []
        native_outputs = run_native_tracker(args.native_runner, args.log) if args.native_runner else {}
        analyze(args.log, samples, window, truth_segments, native_outputs, max(1, args.limit))
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

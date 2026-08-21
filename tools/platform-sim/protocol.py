"""NAMO spatial protocol V1 logical-message validation.

This module is a host-side prototype. It deliberately has no transport,
pairing, Home Assistant, or firmware dependencies.
"""

from __future__ import annotations

import json
import math
import re
from pathlib import Path
from typing import Any, Mapping


PROTOCOL_VERSION = 1
MAX_PARTICIPANTS = 20
MAX_TRACKS_PER_OBSERVATION = 3
MAX_ZONE_EVIDENCE_ITEMS = 6
MAX_ID_LENGTH = 64
MAX_SOFTWARE_VERSION_LENGTH = 32
MIN_TTL_MS = 100
MAX_TTL_MS = 10_000
UINT32_MAX = 0xFFFFFFFF

_ID_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]{0,63}$")
_PARTICIPANT_TYPES = {"namo_node", "satellite"}
_HEALTH_STATES = {"healthy", "degraded", "unavailable"}
_ERROR_SEVERITIES = {"warning", "error"}


class ProtocolValidationError(ValueError):
    def __init__(self, code: str, path: str, message: str):
        super().__init__(f"{code} at {path}: {message}")
        self.code = code
        self.path = path
        self.message = message


def load_fixtures(path: Path | None = None) -> dict[str, Any]:
    fixture_path = path or Path(__file__).with_name("fixtures-v1.json")
    with fixture_path.open("r", encoding="utf-8") as fixture_file:
        return json.load(fixture_file)


def canonical_json(value: Mapping[str, Any]) -> str:
    return json.dumps(
        value,
        allow_nan=False,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    )


def validate_message(kind: str, value: Any, descriptor: Mapping[str, Any] | None = None) -> None:
    validators = {
        "descriptor": validate_descriptor,
        "observation": validate_observation,
        "groupState": validate_group_state,
        "error": validate_error,
    }
    validator = validators.get(kind)
    if validator is None:
        _fail("unsupported_message_kind", "$", f"unsupported message kind: {kind}")
    if kind == "observation":
        validate_observation(value, descriptor)
    else:
        validator(value)


def validate_descriptor(value: Any) -> None:
    message = _object(value, "$")
    _protocol_version(message)
    _identifier(_required(message, "participantId", "$"), "$.participantId")
    participant_type = _enum(
        _required(message, "participantType", "$"), "$.participantType", _PARTICIPANT_TYPES
    )
    _string(_required(message, "product", "$"), "$.product", MAX_ID_LENGTH)
    _string(
        _required(message, "softwareVersion", "$"),
        "$.softwareVersion",
        MAX_SOFTWARE_VERSION_LENGTH,
    )

    capabilities = _object(_required(message, "capabilities", "$"), "$.capabilities")
    point_tracks = _boolean(_required(capabilities, "pointTracks", "$.capabilities"), "$.capabilities.pointTracks")
    _boolean(_required(capabilities, "presence", "$.capabilities"), "$.capabilities.presence")
    _boolean(_required(capabilities, "motion", "$.capabilities"), "$.capabilities.motion")
    for optional in ("range", "zonePresence", "bridge", "velocity", "positionUncertainty"):
        if optional in capabilities:
            _boolean(capabilities[optional], f"$.capabilities.{optional}")
    max_targets = _integer(
        _required(capabilities, "maxTargets", "$.capabilities"),
        "$.capabilities.maxTargets",
        0,
        MAX_TRACKS_PER_OBSERVATION,
    )
    if point_tracks != (max_targets > 0):
        _fail(
            "capability_mismatch",
            "$.capabilities.maxTargets",
            "pointTracks requires maxTargets 1..3; non-spatial participants require zero",
        )

    protocol_range = _object(_required(message, "protocolRange", "$"), "$.protocolRange")
    minimum = _integer(_required(protocol_range, "minimum", "$.protocolRange"), "$.protocolRange.minimum", 1, 255)
    maximum = _integer(_required(protocol_range, "maximum", "$.protocolRange"), "$.protocolRange.maximum", 1, 255)
    if minimum > maximum or not minimum <= PROTOCOL_VERSION <= maximum:
        _fail("unsupported_protocol_version", "$.protocolRange", "range must include protocol version 1")

    if "attestation" in message and participant_type != "namo_node":
        _fail("authority_violation", "$.attestation", "Satellite descriptors cannot claim NAMO device attestation")


def validate_observation(value: Any, descriptor: Mapping[str, Any] | None = None) -> None:
    message = _object(value, "$")
    _protocol_version(message)
    for field in ("siteId", "participantId", "groupId", "bootId"):
        _identifier(_required(message, field, "$"), f"$.{field}")
    participant_type = _enum(
        _required(message, "participantType", "$"), "$.participantType", _PARTICIPANT_TYPES
    )
    _integer(_required(message, "sequence", "$"), "$.sequence", 1, UINT32_MAX)
    _integer(_required(message, "observedAtMs", "$"), "$.observedAtMs", 0, UINT32_MAX)
    ttl_ms = _integer(_required(message, "ttlMs", "$"), "$.ttlMs", MIN_TTL_MS, MAX_TTL_MS)

    frame = _object(_required(message, "frame", "$"), "$.frame")
    if _required(frame, "id", "$.frame") != "sensor-local":
        _fail("unsupported_coordinate_frame", "$.frame.id", "V1 observations use sensor-local coordinates")
    _integer(_required(frame, "poseVersion", "$.frame"), "$.frame.poseVersion", 1, UINT32_MAX)
    _integer(
        _required(frame, "calibrationVersion", "$.frame"),
        "$.frame.calibrationVersion",
        1,
        UINT32_MAX,
    )

    tracks = _list(_required(message, "tracks", "$"), "$.tracks", MAX_TRACKS_PER_OBSERVATION)
    track_ids: set[int] = set()
    for index, track_value in enumerate(tracks):
        path = f"$.tracks[{index}]"
        track = _object(track_value, path)
        _forbid(track, ("roomState", "exitZoneMask", "exitAgeMs"), path)
        local_track_id = _integer(_required(track, "localTrackId", path), f"{path}.localTrackId", 1, UINT32_MAX)
        if local_track_id in track_ids:
            _fail("duplicate_track_id", f"{path}.localTrackId", "localTrackId must be unique within an observation")
        track_ids.add(local_track_id)
        for field in ("xMm", "yMm"):
            _finite_number(_required(track, field, path), f"{path}.{field}", -100_000, 100_000)
        for field in ("vxMmS", "vyMmS"):
            if field in track:
                _finite_number(track[field], f"{path}.{field}", -20_000, 20_000)
        if ("vxMmS" in track) != ("vyMmS" in track):
            _fail("incomplete_track_velocity", path, "vxMmS and vyMmS must be provided together")
        _integer(_required(track, "qualityScore", path), f"{path}.qualityScore", 0, 100)
        if "sampleAgeMs" in track:
            _integer(track["sampleAgeMs"], f"{path}.sampleAgeMs", 0, ttl_ms)
        if "positionVarianceMm2" in track:
            _covariance(track["positionVarianceMm2"], f"{path}.positionVarianceMm2")

    evidence = _object(_required(message, "evidence", "$"), "$.evidence")
    _forbid(evidence, ("staticAssistArmed", "staticAssistActive", "staticAssistExitVeto"), "$.evidence")
    _boolean(_required(evidence, "presence", "$.evidence"), "$.evidence.presence")
    _boolean(_required(evidence, "motion", "$.evidence"), "$.evidence.motion")
    range_mm = _required(evidence, "rangeMm", "$.evidence")
    if range_mm is not None:
        _finite_number(range_mm, "$.evidence.rangeMm", 0, 100_000)
    zone_presence = _list(
        _required(evidence, "zonePresence", "$.evidence"),
        "$.evidence.zonePresence",
        MAX_ZONE_EVIDENCE_ITEMS,
    )
    for index, zone_id in enumerate(zone_presence):
        _identifier(zone_id, f"$.evidence.zonePresence[{index}]")

    health = _object(_required(message, "health", "$"), "$.health")
    _enum(_required(health, "state", "$.health"), "$.health.state", _HEALTH_STATES)
    _integer(_required(health, "sampleAgeMs", "$.health"), "$.health.sampleAgeMs", 0, ttl_ms)

    if descriptor is not None:
        validate_descriptor(descriptor)
        if descriptor["participantId"] != message["participantId"] or descriptor["participantType"] != participant_type:
            _fail("descriptor_mismatch", "$.participantId", "observation identity differs from its descriptor")
        _validate_observation_capabilities(message, descriptor)


def validate_group_state(value: Any) -> None:
    message = _object(value, "$")
    _protocol_version(message)
    for field in ("siteId", "floorplanId", "spaceId", "groupId", "leaderNodeId"):
        _identifier(_required(message, field, "$"), f"$.{field}")
    for field in ("configVersion", "term", "sequence"):
        _integer(_required(message, field, "$"), f"$.{field}", 1, UINT32_MAX)
    _integer(_required(message, "observedAtMs", "$"), "$.observedAtMs", 0, UINT32_MAX)
    _integer(_required(message, "ttlMs", "$"), "$.ttlMs", MIN_TTL_MS, MAX_TTL_MS)
    _boolean(_required(message, "occupancy", "$"), "$.occupancy")
    _boolean(_required(message, "motion", "$"), "$.motion")
    _integer(
        _required(message, "targetCount", "$"),
        "$.targetCount",
        0,
        MAX_PARTICIPANTS * MAX_TRACKS_PER_OBSERVATION,
    )
    zones = _list(_required(message, "zones", "$"), "$.zones", MAX_ZONE_EVIDENCE_ITEMS)
    for index, zone_value in enumerate(zones):
        path = f"$.zones[{index}]"
        zone = _object(zone_value, path)
        _identifier(_required(zone, "zoneId", path), f"{path}.zoneId")
        zone_occupancy = _boolean(_required(zone, "occupancy", path), f"{path}.occupancy")
        zone_target_count = _integer(
            _required(zone, "targetCount", path),
            f"{path}.targetCount",
            0,
            MAX_PARTICIPANTS * MAX_TRACKS_PER_OBSERVATION,
        )
        if (not zone_occupancy and zone_target_count > 0) or zone_target_count > message["targetCount"]:
            _fail("group_state_mismatch", path, "Zone target count conflicts with Group state")
    _enum(_required(message, "health", "$"), "$.health", _HEALTH_STATES)
    online = _integer(_required(message, "participantsOnline", "$"), "$.participantsOnline", 1, MAX_PARTICIPANTS)
    expected = _integer(_required(message, "participantsExpected", "$"), "$.participantsExpected", 1, MAX_PARTICIPANTS)
    if online > expected:
        _fail("participant_count_mismatch", "$.participantsOnline", "online participants cannot exceed expected participants")
    if (not message["occupancy"] and message["targetCount"] > 0) or (message["motion"] and not message["occupancy"]):
        _fail("group_state_mismatch", "$", "motion or spatial targets require Group occupancy")


def validate_error(value: Any) -> None:
    message = _object(value, "$")
    _protocol_version(message)
    _identifier(_required(message, "code", "$"), "$.code")
    _enum(_required(message, "severity", "$"), "$.severity", _ERROR_SEVERITIES)
    _string(_required(message, "message", "$"), "$.message", 160)
    details = _object(_required(message, "details", "$"), "$.details")
    if len(details) > 8:
        _fail("too_many_items", "$.details", "error details are bounded to eight fields")
    for key, detail in details.items():
        _identifier(key, f"$.details.{key}")
        _error_detail(detail, f"$.details.{key}")


def _validate_observation_capabilities(message: Mapping[str, Any], descriptor: Mapping[str, Any]) -> None:
    capabilities = descriptor["capabilities"]
    tracks = message["tracks"]
    evidence = message["evidence"]
    if not capabilities["pointTracks"] and tracks:
        _fail("authority_violation", "$.tracks", "presence-only participants cannot publish spatial tracks")
    if len(tracks) > capabilities["maxTargets"]:
        _fail("capability_mismatch", "$.tracks", "observation exceeds the participant maxTargets capability")
    for index, track in enumerate(tracks):
        path = f"$.tracks[{index}]"
        has_velocity = "vxMmS" in track
        has_uncertainty = "positionVarianceMm2" in track
        if has_velocity != capabilities.get("velocity", False):
            _fail("capability_mismatch", path, "track velocity differs from the declared capability")
        if has_uncertainty != capabilities.get("positionUncertainty", False):
            _fail("capability_mismatch", path, "track uncertainty differs from the declared capability")
    if evidence["presence"] and not capabilities["presence"]:
        _fail("capability_mismatch", "$.evidence.presence", "presence evidence was not declared")
    if evidence["motion"] and not capabilities["motion"]:
        _fail("capability_mismatch", "$.evidence.motion", "motion evidence was not declared")
    if evidence["rangeMm"] is not None and not capabilities.get("range", False):
        _fail("capability_mismatch", "$.evidence.rangeMm", "range evidence was not declared")
    if evidence["zonePresence"] and not capabilities.get("zonePresence", False):
        _fail("capability_mismatch", "$.evidence.zonePresence", "zone evidence was not declared")


def _protocol_version(message: Mapping[str, Any]) -> None:
    version = _integer(_required(message, "protocolVersion", "$"), "$.protocolVersion", 1, 255)
    if version != PROTOCOL_VERSION:
        _fail("unsupported_protocol_version", "$.protocolVersion", "only protocol version 1 is supported")


def _required(value: Mapping[str, Any], field: str, path: str) -> Any:
    if field not in value:
        _fail("missing_required_field", f"{path}.{field}" if path != "$" else f"$.{field}", "field is required")
    return value[field]


def _object(value: Any, path: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        _fail("invalid_type", path, "expected object")
    return value


def _list(value: Any, path: str, maximum: int) -> list[Any]:
    if not isinstance(value, list):
        _fail("invalid_type", path, "expected array")
    if len(value) > maximum:
        _fail("too_many_items", path, f"maximum item count is {maximum}")
    return value


def _string(value: Any, path: str, maximum: int) -> str:
    if not isinstance(value, str) or not value or len(value) > maximum:
        _fail("invalid_string", path, f"expected non-empty string up to {maximum} characters")
    return value


def _identifier(value: Any, path: str) -> str:
    text = _string(value, path, MAX_ID_LENGTH)
    if not _ID_PATTERN.fullmatch(text):
        _fail("invalid_identifier", path, "identifier contains unsupported characters")
    return text


def _boolean(value: Any, path: str) -> bool:
    if not isinstance(value, bool):
        _fail("invalid_type", path, "expected boolean")
    return value


def _integer(value: Any, path: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum or value > maximum:
        _fail("invalid_integer", path, f"expected integer in range {minimum}..{maximum}")
    return value


def _finite_number(value: Any, path: str, minimum: float, maximum: float) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        _fail("invalid_number", path, "expected number")
    number = float(value)
    if not math.isfinite(number) or number < minimum or number > maximum:
        _fail("invalid_number", path, f"expected finite number in range {minimum}..{maximum}")
    return number


def _enum(value: Any, path: str, allowed: set[str]) -> str:
    if not isinstance(value, str) or value not in allowed:
        _fail("invalid_enum", path, f"expected one of: {', '.join(sorted(allowed))}")
    return value


def _covariance(value: Any, path: str) -> None:
    entries = _list(value, path, 4)
    if len(entries) != 4:
        _fail("invalid_covariance", path, "expected [xx, xy, yx, yy]")
    xx, xy, yx, yy = (_finite_number(entry, f"{path}[{index}]", -1e10, 1e10) for index, entry in enumerate(entries))
    if xx < 0 or yy < 0 or not math.isclose(xy, yx, abs_tol=0.001) or xx * yy < xy * yx - 0.001:
        _fail("invalid_covariance", path, "matrix must be symmetric and positive semidefinite")


def _forbid(value: Mapping[str, Any], fields: tuple[str, ...], path: str) -> None:
    for field in fields:
        if field in value:
            _fail("local_policy_field_forbidden", f"{path}.{field}", "local policy state is not wire observation data")


def _error_detail(value: Any, path: str) -> None:
    if value is None or isinstance(value, bool):
        return
    if isinstance(value, str):
        _string(value, path, 160)
        return
    if isinstance(value, (int, float)):
        _finite_number(value, path, -1e12, 1e12)
        return
    _fail("invalid_error_detail", path, "details values must be bounded JSON scalars")


def _fail(code: str, path: str, message: str) -> None:
    raise ProtocolValidationError(code, path, message)

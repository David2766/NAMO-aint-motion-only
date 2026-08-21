"""Bounded latest-observation inbox for host-side Satellite simulations."""

from __future__ import annotations

import copy
from dataclasses import dataclass
from typing import Any, Mapping

from codec import (
    CodecError,
    MAX_WIRE_PAYLOAD_BYTES,
    TRANSPORT_KEY_BYTES,
    decode_authenticated_observation,
    encode_authenticated_observation,
    participant_token,
)
from protocol import MAX_PARTICIPANTS, ProtocolValidationError, validate_descriptor, validate_observation


REORDER_WINDOW = 8
RECENT_BOOT_IDS = 4
TRANSPORT_RATE_WINDOW_MS = 1000
MAX_AUTHENTICATED_OBSERVATIONS_PER_WINDOW = 2


@dataclass(frozen=True)
class IngestResult:
    status: str
    accepted: bool
    queued: bool


@dataclass
class _StreamState:
    boot_id: str
    highest_sequence: int
    recent_sequences: set[int]
    previous_boot_ids: list[str]


@dataclass
class _PendingObservation:
    observation: dict[str, Any]
    expires_at_ms: int


@dataclass(frozen=True)
class _TransportCredential:
    participant_id: str
    participant_token: int
    key_version: int
    key: bytes
    enabled: bool


@dataclass
class _TransportRateState:
    window_started_ms: int
    accepted_packets: int


class SatelliteSimulator:
    """Deterministic sender for logical observations and authenticated packets."""

    def __init__(self, descriptor: Mapping[str, Any], observation_template: Mapping[str, Any]) -> None:
        validate_descriptor(descriptor)
        validate_observation(observation_template, descriptor)
        self.descriptor = copy.deepcopy(dict(descriptor))
        self._template = copy.deepcopy(dict(observation_template))
        self._boot_id = observation_template["bootId"]
        self._sequence = 0

    def emit(self, *, observed_at_ms: int, drop: bool = False) -> dict[str, Any] | None:
        self._sequence += 1
        observation = copy.deepcopy(self._template)
        observation["bootId"] = self._boot_id
        observation["sequence"] = self._sequence
        observation["observedAtMs"] = observed_at_ms
        validate_observation(observation, self.descriptor)
        return None if drop else observation

    def emit_packet(
        self,
        *,
        observed_at_ms: int,
        transport_key: bytes,
        key_version: int = 1,
        drop: bool = False,
    ) -> bytes | None:
        observation = self.emit(observed_at_ms=observed_at_ms, drop=drop)
        if observation is None:
            return None
        return encode_authenticated_observation(
            observation, transport_key, key_version=key_version
        )

    def restart(self, boot_id: str) -> None:
        if not isinstance(boot_id, str) or not boot_id or boot_id == self._boot_id:
            raise ValueError("restart requires a new non-empty bootId")
        self._boot_id = boot_id
        self._sequence = 0


class BoundedObservationInbox:
    """Keeps at most one current observation for each of 20 participants."""

    def __init__(self) -> None:
        self._descriptors: dict[str, dict[str, Any]] = {}
        self._streams: dict[str, _StreamState] = {}
        self._pending: dict[str, _PendingObservation] = {}
        self._credentials: dict[str, _TransportCredential] = {}
        self._transport_rates: dict[str, _TransportRateState] = {}

    @property
    def participant_count(self) -> int:
        return len(self._descriptors)

    @property
    def pending_count(self) -> int:
        return len(self._pending)

    def register(self, descriptor: Mapping[str, Any]) -> None:
        validate_descriptor(descriptor)
        participant_id = descriptor["participantId"]
        previous = self._descriptors.get(participant_id)
        if previous is None and len(self._descriptors) >= MAX_PARTICIPANTS:
            raise ProtocolValidationError(
                "participant_limit_exceeded",
                "$.participantId",
                f"active participant limit is {MAX_PARTICIPANTS}",
            )
        if previous is not None and previous["participantType"] != descriptor["participantType"]:
            raise ProtocolValidationError(
                "descriptor_mismatch",
                "$.participantType",
                "registered participant type cannot change in place",
            )
        self._descriptors[participant_id] = copy.deepcopy(dict(descriptor))

    def unregister(self, participant_id: str) -> None:
        self._descriptors.pop(participant_id, None)
        self._streams.pop(participant_id, None)
        self._pending.pop(participant_id, None)
        self._credentials.pop(participant_id, None)
        self._transport_rates.pop(participant_id, None)

    def set_transport_key(
        self,
        participant_id: str,
        key: bytes,
        *,
        key_version: int = 1,
        enabled: bool = True,
    ) -> None:
        if participant_id not in self._descriptors:
            raise ProtocolValidationError(
                "unknown_participant",
                "$.participantId",
                "transport credentials require a registered participant",
            )
        if isinstance(key_version, bool) or not isinstance(key_version, int) or not 1 <= key_version <= 0xFFFFFFFF:
            raise ProtocolValidationError(
                "invalid_key_version", "$.keyVersion", "key version must be 1..4294967295"
            )
        if not isinstance(key, bytes) or len(key) != TRANSPORT_KEY_BYTES:
            raise ProtocolValidationError(
                "invalid_transport_key",
                "$.transportKey",
                f"transport key must be exactly {TRANSPORT_KEY_BYTES} bytes",
            )
        token = participant_token(participant_id)
        for credential in self._credentials.values():
            if credential.participant_id != participant_id and credential.participant_token == token:
                raise ProtocolValidationError(
                    "participant_token_collision",
                    "$.participantId",
                    "participant token collides with an existing transport credential",
                )
        self._credentials[participant_id] = _TransportCredential(
            participant_id, token, key_version, key, enabled
        )

    def set_transport_enabled(self, participant_id: str, enabled: bool) -> None:
        credential = self._credentials.get(participant_id)
        if credential is None:
            raise ProtocolValidationError(
                "unknown_transport_key",
                "$.participantId",
                "participant has no transport credential",
            )
        self._credentials[participant_id] = _TransportCredential(
            credential.participant_id,
            credential.participant_token,
            credential.key_version,
            credential.key,
            enabled,
        )

    def ingest_packet(
        self,
        packet: bytes,
        *,
        now_ms: int,
        transport_age_ms: int = 0,
    ) -> IngestResult:
        try:
            observation = decode_authenticated_observation(packet, self._resolve_transport_key)
        except CodecError as error:
            return IngestResult(error.code, False, False)
        if isinstance(now_ms, bool) or not isinstance(now_ms, int) or now_ms < 0:
            return IngestResult("invalid_receiver_time", False, False)
        if not self._accept_transport_rate(observation["participantId"], now_ms):
            return IngestResult("transport_rate_limited", False, False)
        return self.ingest(
            observation,
            now_ms=now_ms,
            encoded_size=len(packet),
            transport_age_ms=transport_age_ms,
        )

    def ingest(
        self,
        observation: Mapping[str, Any],
        *,
        now_ms: int,
        encoded_size: int | None = None,
        transport_age_ms: int = 0,
    ) -> IngestResult:
        participant_id = observation.get("participantId") if isinstance(observation, Mapping) else None
        if not isinstance(participant_id, str):
            return IngestResult("invalid_observation", False, False)
        descriptor = self._descriptors.get(participant_id)
        if descriptor is None:
            return IngestResult("unknown_participant", False, False)
        if encoded_size is not None and (encoded_size <= 0 or encoded_size > MAX_WIRE_PAYLOAD_BYTES):
            return IngestResult("payload_too_large", False, False)

        try:
            validate_observation(observation, descriptor)
        except ProtocolValidationError:
            return IngestResult("invalid_observation", False, False)

        if isinstance(now_ms, bool) or not isinstance(now_ms, int) or now_ms < 0:
            return IngestResult("invalid_receiver_time", False, False)
        if isinstance(transport_age_ms, bool) or not isinstance(transport_age_ms, int) or transport_age_ms < 0:
            return IngestResult("invalid_transport_age", False, False)
        consumed_age_ms = observation["health"]["sampleAgeMs"] + transport_age_ms
        if consumed_age_ms >= observation["ttlMs"]:
            return IngestResult("expired_observation", False, False)
        expires_at_ms = now_ms + observation["ttlMs"] - consumed_age_ms

        boot_id = observation["bootId"]
        sequence = observation["sequence"]
        state = self._streams.get(participant_id)
        if state is None:
            self._streams[participant_id] = _StreamState(boot_id, sequence, {sequence}, [])
            self._queue(participant_id, observation, expires_at_ms)
            return IngestResult("accepted", True, True)
        if state.boot_id != boot_id:
            if boot_id in state.previous_boot_ids:
                return IngestResult("replayed_boot", False, False)
            previous_boot_ids = (state.previous_boot_ids + [state.boot_id])[-RECENT_BOOT_IDS:]
            self._streams[participant_id] = _StreamState(boot_id, sequence, {sequence}, previous_boot_ids)
            self._queue(participant_id, observation, expires_at_ms)
            return IngestResult("accepted_restart", True, True)

        if sequence in state.recent_sequences:
            return IngestResult("duplicate_observation", False, False)
        if sequence < state.highest_sequence:
            if state.highest_sequence - sequence <= REORDER_WINDOW:
                state.recent_sequences.add(sequence)
                self._trim_recent(state)
                return IngestResult("reordered_observation_ignored", True, False)
            return IngestResult("stale_sequence", False, False)

        state.highest_sequence = sequence
        state.recent_sequences.add(sequence)
        self._trim_recent(state)
        self._queue(participant_id, observation, expires_at_ms)
        return IngestResult("accepted", True, True)

    def drain(self, *, now_ms: int) -> list[dict[str, Any]]:
        observations = [
            self._pending[key].observation
            for key in sorted(self._pending)
            if now_ms < self._pending[key].expires_at_ms
        ]
        self._pending = {}
        return observations

    def _queue(self, participant_id: str, observation: Mapping[str, Any], expires_at_ms: int) -> None:
        self._pending[participant_id] = _PendingObservation(copy.deepcopy(dict(observation)), expires_at_ms)

    def _resolve_transport_key(self, token: int, key_version: int) -> bytes | None:
        for credential in self._credentials.values():
            if (
                credential.enabled
                and credential.key_version == key_version
                and credential.participant_token == token
            ):
                return credential.key
        return None

    def _accept_transport_rate(self, participant_id: str, now_ms: int) -> bool:
        state = self._transport_rates.get(participant_id)
        if state is None or now_ms - state.window_started_ms >= TRANSPORT_RATE_WINDOW_MS:
            self._transport_rates[participant_id] = _TransportRateState(now_ms, 1)
            return True
        if now_ms < state.window_started_ms:
            return False
        if state.accepted_packets >= MAX_AUTHENTICATED_OBSERVATIONS_PER_WINDOW:
            return False
        state.accepted_packets += 1
        return True

    @staticmethod
    def _trim_recent(state: _StreamState) -> None:
        minimum = max(1, state.highest_sequence - REORDER_WINDOW)
        state.recent_sequences = {sequence for sequence in state.recent_sequences if sequence >= minimum}

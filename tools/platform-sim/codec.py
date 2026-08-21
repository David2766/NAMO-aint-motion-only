"""Bounded host-side codecs and authenticated wire envelope for observations."""

from __future__ import annotations

import hashlib
import hmac
import json
import struct
from typing import Any, Callable, Mapping, TypeAlias

import msgpack

from protocol import canonical_json, validate_observation


MAX_WIRE_PAYLOAD_BYTES = 4096
MAX_AUTHENTICATED_PACKET_BYTES = 768
TRANSPORT_KEY_BYTES = 32
AUTH_TAG_BYTES = 32
AUTHENTICATED_OBSERVATION_MAGIC = b"NAMO"
AUTHENTICATED_OBSERVATION_VERSION = 1
OBSERVATION_MESSAGE_KIND = 1
_AUTH_PREFIX = struct.Struct("!4sBBBBIQHH")
AUTH_PREFIX_BYTES = _AUTH_PREFIX.size
MAX_AUTHENTICATED_PAYLOAD_BYTES = (
    MAX_AUTHENTICATED_PACKET_BYTES - AUTH_PREFIX_BYTES - AUTH_TAG_BYTES
)
ResolvedTransportKey: TypeAlias = bytes | tuple[bytes, str]


class CodecError(ValueError):
    def __init__(self, message: str, code: str = "invalid_payload") -> None:
        super().__init__(message)
        self.code = code


class JsonObservationCodec:
    name = "json"

    @staticmethod
    def encode(observation: Mapping[str, Any]) -> bytes:
        validate_observation(observation)
        payload = canonical_json(observation).encode("utf-8")
        _bounded(payload)
        return payload

    @staticmethod
    def decode(payload: bytes) -> dict[str, Any]:
        _bounded(payload)
        try:
            value = json.loads(
                payload.decode("utf-8"),
                parse_constant=lambda item: (_ for _ in ()).throw(ValueError(item)),
            )
        except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
            raise CodecError("invalid JSON observation payload") from error
        validate_observation(value)
        return value


class MessagePackObservationCodec:
    """Standard MessagePack candidate supported by the bundled ArduinoJson library."""

    name = "msgpack"

    @staticmethod
    def encode(observation: Mapping[str, Any]) -> bytes:
        validate_observation(observation)
        payload = msgpack.packb(dict(observation), use_bin_type=True)
        _bounded(payload)
        return payload

    @staticmethod
    def decode(payload: bytes) -> dict[str, Any]:
        _bounded(payload)
        try:
            value = msgpack.unpackb(payload, raw=False, strict_map_key=True)
        except Exception as error:
            raise CodecError("invalid MessagePack observation payload") from error
        validate_observation(value)
        return value


def available_codecs() -> list[Any]:
    return [JsonObservationCodec, MessagePackObservationCodec]


def participant_token(participant_id: str) -> int:
    if not isinstance(participant_id, str) or not participant_id:
        raise CodecError("participantId must be a non-empty string", "invalid_participant")
    digest = hashlib.sha256(participant_id.encode("utf-8")).digest()
    return int.from_bytes(digest[:8], "big")


def encode_authenticated_observation(
    observation: Mapping[str, Any],
    transport_key: bytes,
    *,
    key_version: int = 1,
    participant_token_value: int | None = None,
) -> bytes:
    key = _transport_key(transport_key)
    if isinstance(key_version, bool) or not isinstance(key_version, int) or not 1 <= key_version <= 0xFFFFFFFF:
        raise CodecError("key version must be in range 1..4294967295", "invalid_key_version")
    token = (
        participant_token(observation["participantId"])
        if participant_token_value is None
        else participant_token_value
    )
    if isinstance(token, bool) or not isinstance(token, int) or not 1 <= token <= 0xFFFFFFFFFFFFFFFF:
        raise CodecError("participant token must be in range 1..2^64-1", "invalid_participant_token")

    payload = MessagePackObservationCodec.encode(observation)
    _bounded(payload, MAX_AUTHENTICATED_PAYLOAD_BYTES)
    prefix = _AUTH_PREFIX.pack(
        AUTHENTICATED_OBSERVATION_MAGIC,
        AUTHENTICATED_OBSERVATION_VERSION,
        OBSERVATION_MESSAGE_KIND,
        0,
        AUTH_PREFIX_BYTES,
        key_version,
        token,
        len(payload),
        0,
    )
    tag = hmac.digest(key, prefix + payload, "sha256")
    packet = prefix + payload + tag
    _bounded(packet, MAX_AUTHENTICATED_PACKET_BYTES)
    return packet


def decode_authenticated_observation(
    packet: bytes,
    key_resolver: Callable[[int, int], ResolvedTransportKey | None],
) -> dict[str, Any]:
    _bounded(packet, MAX_AUTHENTICATED_PACKET_BYTES)
    minimum_size = AUTH_PREFIX_BYTES + 1 + AUTH_TAG_BYTES
    if len(packet) < minimum_size:
        raise CodecError("authenticated packet is truncated", "invalid_envelope")

    try:
        magic, version, kind, flags, header_size, key_version, token, payload_size, reserved = (
            _AUTH_PREFIX.unpack_from(packet)
        )
    except struct.error as error:
        raise CodecError("authenticated packet header is invalid", "invalid_envelope") from error

    if magic != AUTHENTICATED_OBSERVATION_MAGIC:
        raise CodecError("authenticated packet magic is invalid", "invalid_envelope")
    if version != AUTHENTICATED_OBSERVATION_VERSION:
        raise CodecError("authenticated packet version is unsupported", "unsupported_wire_version")
    if kind != OBSERVATION_MESSAGE_KIND:
        raise CodecError("authenticated packet kind is unsupported", "unsupported_message_kind")
    if flags != 0 or header_size != AUTH_PREFIX_BYTES or reserved != 0 or key_version == 0:
        raise CodecError("authenticated packet header fields are invalid", "invalid_envelope")
    if payload_size == 0 or payload_size > MAX_AUTHENTICATED_PAYLOAD_BYTES:
        raise CodecError("authenticated payload size is invalid", "invalid_envelope")
    if len(packet) != AUTH_PREFIX_BYTES + payload_size + AUTH_TAG_BYTES:
        raise CodecError("authenticated packet length does not match its header", "invalid_envelope")

    resolved = key_resolver(token, key_version)
    if resolved is None:
        raise CodecError("transport key is not available", "unknown_transport_key")
    expected_participant_id: str | None = None
    if isinstance(resolved, tuple):
        transport_key, expected_participant_id = resolved
    else:
        transport_key = resolved
    key = _transport_key(transport_key)
    payload_end = AUTH_PREFIX_BYTES + payload_size
    signed = packet[:payload_end]
    supplied_tag = packet[payload_end:]
    expected_tag = hmac.digest(key, signed, "sha256")
    if not hmac.compare_digest(supplied_tag, expected_tag):
        raise CodecError("authenticated packet signature is invalid", "authentication_failed")

    observation = MessagePackObservationCodec.decode(packet[AUTH_PREFIX_BYTES:payload_end])
    participant_id = observation["participantId"]
    if (
        expected_participant_id is not None
        and participant_id != expected_participant_id
    ) or (
        expected_participant_id is None
        and participant_token(participant_id) != token
    ):
        raise CodecError("authenticated participant token does not match payload", "participant_token_mismatch")
    return observation


def _transport_key(value: bytes) -> bytes:
    if not isinstance(value, bytes) or len(value) != TRANSPORT_KEY_BYTES:
        raise CodecError(
            f"transport key must be exactly {TRANSPORT_KEY_BYTES} bytes",
            "invalid_transport_key",
        )
    return value


def _bounded(payload: bytes, maximum: int = MAX_WIRE_PAYLOAD_BYTES) -> None:
    if not isinstance(payload, bytes):
        raise CodecError("payload must be bytes")
    if not payload or len(payload) > maximum:
        code = "packet_too_large" if len(payload) > maximum else "invalid_payload"
        raise CodecError(f"payload size must be 1..{maximum} bytes", code)

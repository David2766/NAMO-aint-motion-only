from __future__ import annotations

import copy
import json
from pathlib import Path
import unittest
from unittest import mock

from benchmark import benchmark
from codec import (
    AUTH_PREFIX_BYTES,
    AUTH_TAG_BYTES,
    CodecError,
    JsonObservationCodec,
    MAX_AUTHENTICATED_PACKET_BYTES,
    MAX_AUTHENTICATED_PAYLOAD_BYTES,
    MessagePackObservationCodec,
    MAX_WIRE_PAYLOAD_BYTES,
    decode_authenticated_observation,
    encode_authenticated_observation,
    participant_token,
)
from protocol import MAX_PARTICIPANTS, ProtocolValidationError, load_fixtures
import simulator as simulator_module
from simulator import BoundedObservationInbox, SatelliteSimulator


class CodecTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.fixtures = load_fixtures()

    def test_json_and_messagepack_round_trip_valid_observations(self) -> None:
        for observation in self.fixtures["valid"]["observations"]:
            for codec in (JsonObservationCodec, MessagePackObservationCodec):
                with self.subTest(codec=codec.name, participant=observation["participantId"]):
                    decoded = codec.decode(codec.encode(observation))
                    self.assertEqual(decoded["participantId"], observation["participantId"])
                    self.assertEqual(decoded["tracks"], observation["tracks"])
                    self.assertEqual(decoded["evidence"], observation["evidence"])

    def test_messagepack_rejects_truncation_and_trailing_bytes(self) -> None:
        observation = self.fixtures["valid"]["observations"][0]
        payload = MessagePackObservationCodec.encode(observation)
        with self.assertRaises(CodecError):
            MessagePackObservationCodec.decode(payload[:-1])
        with self.assertRaises(CodecError):
            MessagePackObservationCodec.decode(payload + b"\x00")

    def test_codecs_reject_oversized_payload_before_parsing(self) -> None:
        payload = b"x" * (MAX_WIRE_PAYLOAD_BYTES + 1)
        for codec in (JsonObservationCodec, MessagePackObservationCodec):
            with self.subTest(codec=codec.name):
                with self.assertRaises(CodecError):
                    codec.decode(payload)

    def test_benchmark_smoke_covers_all_available_codecs(self) -> None:
        result = benchmark(iterations=2)
        self.assertEqual(set(result["scenarios"]), {"presence-only", "one-track", "three-track"})
        for measurements in result["scenarios"].values():
            self.assertIn("json", measurements)
            self.assertIn("msgpack", measurements)
            self.assertGreater(measurements["json"]["bytes"], 0)

    def test_authenticated_observation_round_trips_within_embedded_budget(self) -> None:
        observation = copy.deepcopy(self.fixtures["valid"]["observations"][0])
        base = observation["tracks"][0]
        observation["tracks"] = []
        for index in range(3):
            track = copy.deepcopy(base)
            track["localTrackId"] = index + 1
            observation["tracks"].append(track)

        key = bytes(range(32))
        token = participant_token(observation["participantId"])
        packet = encode_authenticated_observation(observation, key, key_version=7)
        decoded = decode_authenticated_observation(
            packet,
            lambda received_token, version: key
            if received_token == token and version == 7
            else None,
        )

        self.assertEqual(decoded, observation)
        self.assertLessEqual(len(packet), MAX_AUTHENTICATED_PACKET_BYTES)
        self.assertLessEqual(
            len(packet) - AUTH_PREFIX_BYTES - AUTH_TAG_BYTES,
            MAX_AUTHENTICATED_PAYLOAD_BYTES,
        )

    def test_authentication_rejects_unknown_key_wrong_key_and_tampering(self) -> None:
        observation = self.fixtures["valid"]["observations"][0]
        key = bytes(range(32))
        packet = encode_authenticated_observation(observation, key)

        self.assert_codec_error(
            "unknown_transport_key",
            decode_authenticated_observation,
            packet,
            lambda _token, _version: None,
        )
        self.assert_codec_error(
            "authentication_failed",
            decode_authenticated_observation,
            packet,
            lambda _token, _version: b"x" * 32,
        )
        tampered = bytearray(packet)
        tampered[AUTH_PREFIX_BYTES + 4] ^= 0x01
        self.assert_codec_error(
            "authentication_failed",
            decode_authenticated_observation,
            bytes(tampered),
            lambda _token, _version: key,
        )

    def test_authenticated_envelope_supports_pair_scoped_opaque_token(self) -> None:
        observation = self.fixtures["valid"]["observations"][0]
        key = bytes(range(32))
        opaque_token = 0x0102030405060708
        packet = encode_authenticated_observation(
            observation,
            key,
            participant_token_value=opaque_token,
        )
        decoded = decode_authenticated_observation(
            packet,
            lambda token, version: (
                key,
                observation["participantId"],
            )
            if token == opaque_token and version == 1
            else None,
        )
        self.assertEqual(decoded, observation)
        self.assert_codec_error(
            "participant_token_mismatch",
            decode_authenticated_observation,
            packet,
            lambda _token, _version: (key, "different-node"),
        )

    def test_authenticated_envelope_rejects_truncation_and_oversize_before_decode(self) -> None:
        observation = self.fixtures["valid"]["observations"][0]
        key = bytes(range(32))
        packet = encode_authenticated_observation(observation, key)
        self.assert_codec_error(
            "invalid_envelope",
            decode_authenticated_observation,
            packet[:AUTH_PREFIX_BYTES],
            lambda _token, _version: key,
        )
        self.assert_codec_error(
            "packet_too_large",
            decode_authenticated_observation,
            b"x" * (MAX_AUTHENTICATED_PACKET_BYTES + 1),
            lambda _token, _version: key,
        )

    def test_authenticated_packet_matches_golden_vector(self) -> None:
        vector_path = Path(__file__).with_name("transport-v1-golden.json")
        vector = json.loads(vector_path.read_text(encoding="utf-8"))
        observation = self.fixtures["valid"]["observations"][vector["observationIndex"]]
        key = bytes.fromhex(vector["transportKeyHex"])
        packet = encode_authenticated_observation(
            observation, key, key_version=vector["keyVersion"]
        )
        golden_packet = bytes.fromhex(vector["packetHex"])
        self.assertEqual(packet, golden_packet)
        self.assertEqual(
            participant_token(observation["participantId"]),
            int(vector["participantTokenHex"], 16),
        )
        decoded = decode_authenticated_observation(
            golden_packet,
            lambda token, version: key
            if token == int(vector["participantTokenHex"], 16)
            and version == vector["keyVersion"]
            else None,
        )
        self.assertEqual(decoded, observation)

    def assert_codec_error(self, code: str, function, *args) -> None:
        with self.assertRaises(CodecError) as raised:
            function(*args)
        self.assertEqual(raised.exception.code, code)


class SimulatorTests(unittest.TestCase):
    def setUp(self) -> None:
        fixtures = load_fixtures()
        self.descriptor = copy.deepcopy(fixtures["valid"]["descriptors"][0])
        self.observation = copy.deepcopy(fixtures["valid"]["observations"][0])
        self.inbox = BoundedObservationInbox()
        self.inbox.register(self.descriptor)

    def test_latest_observation_replaces_pending_without_growing_queue(self) -> None:
        first = self.inbox.ingest(self.observation, now_ms=self.observation["observedAtMs"])
        newer = copy.deepcopy(self.observation)
        newer["sequence"] += 1
        newer["observedAtMs"] += 100
        second = self.inbox.ingest(newer, now_ms=newer["observedAtMs"])
        self.assertTrue(first.queued)
        self.assertTrue(second.queued)
        self.assertEqual(self.inbox.pending_count, 1)
        self.assertEqual(self.inbox.drain(now_ms=newer["observedAtMs"])[0]["sequence"], newer["sequence"])

    def test_duplicate_reorder_and_restart_do_not_regress_current_sample(self) -> None:
        self.inbox.ingest(self.observation, now_ms=self.observation["observedAtMs"])
        duplicate = self.inbox.ingest(self.observation, now_ms=self.observation["observedAtMs"])
        self.assertEqual(duplicate.status, "duplicate_observation")

        newer = copy.deepcopy(self.observation)
        newer["sequence"] += 2
        newer["observedAtMs"] += 200
        self.inbox.ingest(newer, now_ms=newer["observedAtMs"])
        reordered = copy.deepcopy(newer)
        reordered["sequence"] -= 1
        reordered["observedAtMs"] -= 50
        result = self.inbox.ingest(reordered, now_ms=newer["observedAtMs"])
        self.assertEqual(result.status, "reordered_observation_ignored")
        self.assertFalse(result.queued)
        self.assertEqual(self.inbox.drain(now_ms=newer["observedAtMs"])[0]["sequence"], newer["sequence"])

        restarted = copy.deepcopy(newer)
        restarted["bootId"] = "boot-restarted"
        restarted["sequence"] = 1
        restarted["observedAtMs"] += 100
        result = self.inbox.ingest(restarted, now_ms=restarted["observedAtMs"])
        self.assertEqual(result.status, "accepted_restart")
        self.assertEqual(self.inbox.drain(now_ms=restarted["observedAtMs"])[0]["sequence"], 1)

        old_boot = copy.deepcopy(restarted)
        old_boot["bootId"] = self.observation["bootId"]
        old_boot["sequence"] = newer["sequence"] + 10
        self.assertEqual(
            self.inbox.ingest(old_boot, now_ms=restarted["observedAtMs"]).status,
            "replayed_boot",
        )

    def test_age_unknown_and_oversized_inputs_are_handled_without_clock_offset_assumptions(self) -> None:
        expired = self.inbox.ingest(self.observation, now_ms=50, transport_age_ms=self.observation["ttlMs"])
        self.assertEqual(expired.status, "expired_observation")

        # Sender monotonic time is deliberately unrelated to receiver monotonic time.
        unrelated_clock = copy.deepcopy(self.observation)
        unrelated_clock["observedAtMs"] = 0xFFFFFFF0
        self.assertTrue(self.inbox.ingest(unrelated_clock, now_ms=50).accepted)

        unknown = copy.deepcopy(self.observation)
        unknown["participantId"] = "namo-unknown"
        self.assertEqual(self.inbox.ingest(unknown, now_ms=unknown["observedAtMs"]).status, "unknown_participant")
        self.assertEqual(
            self.inbox.ingest(
                self.observation,
                now_ms=self.observation["observedAtMs"],
                encoded_size=MAX_WIRE_PAYLOAD_BYTES + 1,
            ).status,
            "payload_too_large",
        )
        self.assertEqual(self.inbox.pending_count, 1)

    def test_pending_observation_expires_on_receiver_clock(self) -> None:
        observation = copy.deepcopy(self.observation)
        observation["health"]["sampleAgeMs"] = 100
        self.inbox.ingest(observation, now_ms=500)
        self.assertEqual(self.inbox.drain(now_ms=500 + observation["ttlMs"] - 100), [])

    def test_sender_simulates_packet_loss_reordering_and_restart(self) -> None:
        sender = SatelliteSimulator(self.descriptor, self.observation)
        first = sender.emit(observed_at_ms=100)
        self.assertIsNotNone(first)
        self.assertIsNone(sender.emit(observed_at_ms=200, drop=True))
        third = sender.emit(observed_at_ms=300)
        self.assertIsNotNone(third)
        self.assertEqual(third["sequence"], 3)

        # A sequence gap is packet loss, not a reason to reject the newest sample.
        self.assertTrue(self.inbox.ingest(third, now_ms=30).accepted)
        reordered = self.inbox.ingest(first, now_ms=40)
        self.assertEqual(reordered.status, "reordered_observation_ignored")

        sender.restart("boot-after-reset")
        restarted = sender.emit(observed_at_ms=5)
        self.assertIsNotNone(restarted)
        result = self.inbox.ingest(restarted, now_ms=50)
        self.assertEqual(result.status, "accepted_restart")

    def test_one_two_and_twenty_participants_remain_bounded(self) -> None:
        inbox = BoundedObservationInbox()
        for index in range(MAX_PARTICIPANTS):
            descriptor = copy.deepcopy(self.descriptor)
            descriptor["participantId"] = f"namo-{index + 1}"
            inbox.register(descriptor)
            observation = copy.deepcopy(self.observation)
            observation["participantId"] = descriptor["participantId"]
            observation["bootId"] = f"boot-{index + 1}"
            result = inbox.ingest(observation, now_ms=observation["observedAtMs"])
            self.assertTrue(result.accepted)
        self.assertEqual(inbox.participant_count, MAX_PARTICIPANTS)
        self.assertEqual(inbox.pending_count, MAX_PARTICIPANTS)
        self.assertEqual(len(inbox.drain(now_ms=self.observation["observedAtMs"])), MAX_PARTICIPANTS)

        extra = copy.deepcopy(self.descriptor)
        extra["participantId"] = "namo-21"
        with self.assertRaises(ProtocolValidationError) as raised:
            inbox.register(extra)
        self.assertEqual(raised.exception.code, "participant_limit_exceeded")

    def test_authenticated_ingest_requires_an_enabled_registered_key(self) -> None:
        key = bytes(range(32))
        packet = encode_authenticated_observation(self.observation, key)
        self.assertEqual(
            self.inbox.ingest_packet(
                packet, now_ms=self.observation["observedAtMs"]
            ).status,
            "unknown_transport_key",
        )

        self.inbox.set_transport_key(self.descriptor["participantId"], key)
        accepted = self.inbox.ingest_packet(
            packet, now_ms=self.observation["observedAtMs"]
        )
        self.assertTrue(accepted.accepted)

        self.inbox.set_transport_enabled(self.descriptor["participantId"], False)
        newer = copy.deepcopy(self.observation)
        newer["sequence"] += 1
        disabled_packet = encode_authenticated_observation(newer, key)
        self.assertEqual(
            self.inbox.ingest_packet(
                disabled_packet, now_ms=newer["observedAtMs"]
            ).status,
            "unknown_transport_key",
        )

    def test_transport_key_registration_rejects_participant_token_collision(self) -> None:
        key = bytes(range(32))
        first_id = self.descriptor["participantId"]
        self.inbox.set_transport_key(first_id, key)
        second = copy.deepcopy(self.descriptor)
        second["participantId"] = "namo-token-collision"
        self.inbox.register(second)

        with mock.patch.object(
            simulator_module,
            "participant_token",
            return_value=participant_token(first_id),
        ):
            with self.assertRaises(ProtocolValidationError) as raised:
                self.inbox.set_transport_key(second["participantId"], b"z" * 32)
        self.assertEqual(raised.exception.code, "participant_token_collision")

    def test_authenticated_ingest_applies_ttl_replay_and_restart_rules(self) -> None:
        key = bytes(range(32))
        self.inbox.set_transport_key(self.descriptor["participantId"], key)
        packet = encode_authenticated_observation(self.observation, key)
        first = self.inbox.ingest_packet(
            packet, now_ms=self.observation["observedAtMs"]
        )
        self.assertTrue(first.accepted)
        self.assertEqual(
            self.inbox.ingest_packet(
                packet, now_ms=self.observation["observedAtMs"]
            ).status,
            "duplicate_observation",
        )

        restarted = copy.deepcopy(self.observation)
        restarted["bootId"] = "boot-auth-restarted"
        restarted["sequence"] = 1
        restarted_packet = encode_authenticated_observation(restarted, key)
        self.assertEqual(
            self.inbox.ingest_packet(
                restarted_packet, now_ms=restarted["observedAtMs"] + 1000
            ).status,
            "accepted_restart",
        )

        expired = copy.deepcopy(restarted)
        expired["sequence"] = 2
        expired_packet = encode_authenticated_observation(expired, key)
        self.assertEqual(
            self.inbox.ingest_packet(
                expired_packet,
                now_ms=expired["observedAtMs"] + 1000,
                transport_age_ms=expired["ttlMs"],
            ).status,
            "expired_observation",
        )

    def test_authenticated_transport_limits_each_participant_to_two_packets_per_second(self) -> None:
        key = bytes(range(32))
        self.inbox.set_transport_key(self.descriptor["participantId"], key)
        base_time = self.observation["observedAtMs"]
        for sequence, offset in ((1, 0), (2, 100)):
            sample = copy.deepcopy(self.observation)
            sample["sequence"] = sequence
            packet = encode_authenticated_observation(sample, key)
            self.assertTrue(
                self.inbox.ingest_packet(packet, now_ms=base_time + offset).accepted
            )

        third = copy.deepcopy(self.observation)
        third["sequence"] = 3
        third_packet = encode_authenticated_observation(third, key)
        self.assertEqual(
            self.inbox.ingest_packet(third_packet, now_ms=base_time + 200).status,
            "transport_rate_limited",
        )

        fourth = copy.deepcopy(self.observation)
        fourth["sequence"] = 4
        fourth_packet = encode_authenticated_observation(fourth, key)
        self.assertTrue(
            self.inbox.ingest_packet(fourth_packet, now_ms=base_time + 1000).accepted
        )

    def test_twenty_authenticated_participants_at_two_hz_remain_latest_wins(self) -> None:
        inbox = BoundedObservationInbox()
        keys: dict[str, bytes] = {}
        observations: dict[str, dict] = {}
        for index in range(MAX_PARTICIPANTS):
            participant_id = f"namo-auth-{index + 1}"
            descriptor = copy.deepcopy(self.descriptor)
            descriptor["participantId"] = participant_id
            observation = copy.deepcopy(self.observation)
            observation["participantId"] = participant_id
            observation["bootId"] = f"boot-auth-{index + 1}"
            key = bytes([index + 1]) * 32
            inbox.register(descriptor)
            inbox.set_transport_key(participant_id, key)
            keys[participant_id] = key
            observations[participant_id] = observation

        for sequence in (1, 2):
            for participant_id, observation in observations.items():
                sample = copy.deepcopy(observation)
                sample["sequence"] = sequence
                sample["observedAtMs"] += (sequence - 1) * 500
                packet = encode_authenticated_observation(
                    sample, keys[participant_id]
                )
                result = inbox.ingest_packet(
                    packet, now_ms=sample["observedAtMs"]
                )
                self.assertTrue(result.accepted)

        self.assertEqual(inbox.participant_count, MAX_PARTICIPANTS)
        self.assertEqual(inbox.pending_count, MAX_PARTICIPANTS)
        drained = inbox.drain(
            now_ms=self.observation["observedAtMs"] + 500
        )
        self.assertEqual(len(drained), MAX_PARTICIPANTS)
        self.assertTrue(all(item["sequence"] == 2 for item in drained))


if __name__ == "__main__":
    unittest.main()

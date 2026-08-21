from __future__ import annotations

import copy
import json
import math
import unittest

from protocol import (
    MAX_PARTICIPANTS,
    MAX_TRACKS_PER_OBSERVATION,
    ProtocolValidationError,
    canonical_json,
    load_fixtures,
    validate_descriptor,
    validate_error,
    validate_group_state,
    validate_message,
    validate_observation,
)


class ProtocolFixtureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.fixtures = load_fixtures()

    def test_every_valid_fixture_passes(self) -> None:
        valid = self.fixtures["valid"]
        for descriptor in valid["descriptors"]:
            validate_descriptor(descriptor)
        validate_observation(valid["observations"][0], valid["descriptors"][0])
        validate_observation(valid["observations"][1], valid["descriptors"][1])
        for group_state in valid["groupStates"]:
            validate_group_state(group_state)
        for error in valid["errors"]:
            validate_error(error)

    def test_canonical_json_is_stable_and_round_trips(self) -> None:
        observation = self.fixtures["valid"]["observations"][0]
        encoded = canonical_json(observation)
        self.assertEqual(json.loads(encoded), observation)
        self.assertEqual(encoded, canonical_json(json.loads(encoded)))

    def test_canonical_json_rejects_non_finite_values(self) -> None:
        with self.assertRaises(ValueError):
            canonical_json({"invalid": math.nan})

    def test_invalid_fixture_catalog_has_expected_errors(self) -> None:
        for case in self.fixtures["invalid"]:
            value, descriptor = self._invalid_case(case)
            with self.subTest(case=case["name"]):
                with self.assertRaises(ProtocolValidationError) as raised:
                    validate_message(case["kind"], value, descriptor)
                self.assertEqual(raised.exception.code, case["errorCode"])

    def test_missing_required_identity_is_rejected(self) -> None:
        observation = copy.deepcopy(self.fixtures["valid"]["observations"][0])
        del observation["bootId"]
        self.assert_error("missing_required_field", validate_observation, observation)

    def test_boolean_is_not_accepted_as_integer(self) -> None:
        observation = copy.deepcopy(self.fixtures["valid"]["observations"][0])
        observation["sequence"] = True
        self.assert_error("invalid_integer", validate_observation, observation)

    def test_duplicate_track_identity_is_rejected(self) -> None:
        observation = copy.deepcopy(self.fixtures["valid"]["observations"][0])
        duplicate = copy.deepcopy(observation["tracks"][0])
        observation["tracks"].append(duplicate)
        self.assert_error("duplicate_track_id", validate_observation, observation)

    def test_invalid_covariance_is_rejected(self) -> None:
        observation = copy.deepcopy(self.fixtures["valid"]["observations"][0])
        observation["tracks"][0]["positionVarianceMm2"] = [1, 2, 2, 1]
        self.assert_error("invalid_covariance", validate_observation, observation)

    def test_sample_age_cannot_outlive_message_ttl(self) -> None:
        observation = copy.deepcopy(self.fixtures["valid"]["observations"][0])
        observation["health"]["sampleAgeMs"] = observation["ttlMs"] + 1
        self.assert_error("invalid_integer", validate_observation, observation)

    def test_observation_identity_must_match_descriptor(self) -> None:
        observation = copy.deepcopy(self.fixtures["valid"]["observations"][0])
        descriptor = copy.deepcopy(self.fixtures["valid"]["descriptors"][0])
        observation["participantId"] = "namo-other"
        self.assert_error("descriptor_mismatch", validate_observation, observation, descriptor)

    def test_undeclared_range_evidence_is_rejected(self) -> None:
        observation = copy.deepcopy(self.fixtures["valid"]["observations"][0])
        descriptor = copy.deepcopy(self.fixtures["valid"]["descriptors"][0])
        descriptor["capabilities"]["range"] = False
        self.assert_error("capability_mismatch", validate_observation, observation, descriptor)

    def test_track_metadata_must_match_declared_capabilities(self) -> None:
        observation = copy.deepcopy(self.fixtures["valid"]["observations"][0])
        descriptor = copy.deepcopy(self.fixtures["valid"]["descriptors"][0])
        descriptor["capabilities"]["positionUncertainty"] = False
        self.assert_error("capability_mismatch", validate_observation, observation, descriptor)

        descriptor = copy.deepcopy(self.fixtures["valid"]["descriptors"][0])
        del observation["tracks"][0]["vxMmS"]
        del observation["tracks"][0]["vyMmS"]
        self.assert_error("capability_mismatch", validate_observation, observation, descriptor)

    def test_error_details_reject_nested_unbounded_data(self) -> None:
        error = copy.deepcopy(self.fixtures["valid"]["errors"][0])
        error["details"]["payload"] = {"nested": "x" * 1000}
        self.assert_error("invalid_error_detail", validate_error, error)

    def test_twenty_participant_group_is_valid_and_twenty_first_is_not(self) -> None:
        state = copy.deepcopy(self.fixtures["valid"]["groupStates"][0])
        state["participantsOnline"] = MAX_PARTICIPANTS
        state["participantsExpected"] = MAX_PARTICIPANTS
        validate_group_state(state)
        state["participantsExpected"] = MAX_PARTICIPANTS + 1
        self.assert_error("invalid_integer", validate_group_state, state)

    def test_three_tracks_are_valid_and_four_are_not(self) -> None:
        observation = copy.deepcopy(self.fixtures["valid"]["observations"][0])
        base = observation["tracks"][0]
        observation["tracks"] = []
        for index in range(MAX_TRACKS_PER_OBSERVATION):
            track = copy.deepcopy(base)
            track["localTrackId"] = index + 1
            observation["tracks"].append(track)
        validate_observation(observation)
        fourth = copy.deepcopy(base)
        fourth["localTrackId"] = 4
        observation["tracks"].append(fourth)
        self.assert_error("too_many_items", validate_observation, observation)

    def test_group_state_rejects_targets_without_occupancy(self) -> None:
        state = copy.deepcopy(self.fixtures["valid"]["groupStates"][0])
        state["occupancy"] = False
        self.assert_error("group_state_mismatch", validate_group_state, state)

    def _invalid_case(self, case: dict) -> tuple[dict, dict | None]:
        valid = self.fixtures["valid"]
        if case["kind"] == "descriptor":
            value = copy.deepcopy(valid["descriptors"][case["descriptorIndex"]])
            descriptor = None
        else:
            value = copy.deepcopy(valid["observations"][case["observationIndex"]])
            descriptor = (
                copy.deepcopy(valid["descriptors"][case["descriptorIndex"]])
                if "descriptorIndex" in case
                else None
            )
        self._deep_merge(value, case.get("patch", {}))
        if case.get("specialPatch") == "nonFiniteTrackX":
            value["tracks"][0]["xMm"] = math.nan
        if case.get("specialPatch") == "fourTracks":
            base = copy.deepcopy(value["tracks"][0])
            value["tracks"] = []
            for index in range(4):
                track = copy.deepcopy(base)
                track["localTrackId"] = index + 1
                value["tracks"].append(track)
        return value, descriptor

    @staticmethod
    def _deep_merge(target: dict, patch: dict) -> None:
        for key, value in patch.items():
            if isinstance(value, dict) and isinstance(target.get(key), dict):
                ProtocolFixtureTests._deep_merge(target[key], value)
            else:
                target[key] = copy.deepcopy(value)

    def assert_error(self, code: str, function, *args) -> None:
        with self.assertRaises(ProtocolValidationError) as raised:
            function(*args)
        self.assertEqual(raised.exception.code, code)


if __name__ == "__main__":
    unittest.main()

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "shadow_fusion_compare.py"
SPEC = importlib.util.spec_from_file_location("shadow_fusion_compare", SCRIPT)
compare = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = compare
SPEC.loader.exec_module(compare)


def production_target(target_id="target-1", x=1200, y=2300, active=True):
    return {
        "id": target_id,
        "x": x,
        "y": y,
        "active": active,
    }


def state_fixture(presence=True, target_count=1, targets=None):
    return {
        "connected": True,
        "presence": presence,
        "targetCount": target_count,
        "targets": (
            [production_target()]
            if targets is None and target_count > 0
            else ([] if targets is None else targets)
        ),
    }


def shadow_track(
    track_id=1,
    state="confirmed",
    source_count=2,
    x_mm=1234.0,
    y_mm=2345.0,
):
    return {
        "id": track_id,
        "state": state,
        "sourceCount": source_count,
        "confidence": 94,
        "correlatedViews": False,
        "xMm": x_mm,
        "yMm": y_mm,
        "vxMmS": 10.0,
        "vyMmS": 20.0,
        "varianceXMillimeters2": 20000.0,
        "varianceYMillimeters2": 21000.0,
    }


def shadow_diagnostics(
    candidate_pairs=1,
    paired_measurements=1,
    ambiguous_pairs=0,
):
    return {
        "validInputs": 2,
        "candidatePairs": candidate_pairs,
        "pairedMeasurements": paired_measurements,
        "ambiguousPairs": ambiguous_pairs,
        "droppedMeasurements": 0,
        "tentativeTracks": 0,
        "confirmedTracks": 1,
        "coastingTracks": 0,
    }


def groups_fixture(
    state="active",
    tracks=None,
    max_cycle_us=420,
    diagnostics=None,
):
    tracks = (
        [shadow_track()] if tracks is None else tracks
    )
    return {
        "shadowFusion": {
            "state": state,
            "trackCount": len(tracks),
            "maxCycleUs": max_cycle_us,
            "diagnostics": diagnostics or shadow_diagnostics(),
            "tracks": tracks,
        }
    }


class FakeClient:
    def __init__(self, state=None, groups=None):
        self.state = state or state_fixture()
        self.groups = groups or groups_fixture()
        self.paths = []

    def get_json(self, path):
        self.paths.append(path)
        if path == "/api/state":
            return self.state
        if path == "/api/platform/groups":
            return self.groups
        raise AssertionError(f"unexpected path: {path}")


class ShadowFusionCompareTests(unittest.TestCase):
    def test_parser_compares_only_confirmed_and_coasting_tracks(self):
        sample = compare.parse_sample(
            state_fixture(target_count=2),
            groups_fixture(
                tracks=[
                    shadow_track(1, "confirmed", 2, 1200, 2300),
                    shadow_track(2, "coasting", 1, 1800, 2600),
                    shadow_track(3, "tentative", 1, 4000, 5000),
                ]
            ),
            captured_at="2026-07-27T00:00:00+00:00",
        )
        self.assertTrue(sample.shadow_presence)
        self.assertEqual(sample.shadow_target_count, 2)
        self.assertEqual(sample.shadow_track_count, 3)
        self.assertEqual(len(sample.production_targets), 1)
        self.assertEqual(sample.production_targets[0].x_mm, 1200)
        self.assertEqual(len(sample.shadow_tracks), 3)
        self.assertEqual(sample.admitted_single_source_tracks, 1)
        self.assertEqual(sample.admitted_multi_source_tracks, 1)
        self.assertGreater(sample.max_shadow_track_separation_mm, 600)
        self.assertFalse(sample.presence_mismatch)
        self.assertFalse(sample.target_count_mismatch)

    def test_summary_excludes_inactive_shadow_samples(self):
        samples = [
            compare.parse_sample(
                state_fixture(True, 1), groups_fixture("active")
            ),
            compare.parse_sample(
                state_fixture(True, 1),
                groups_fixture("not_group_leader", tracks=[]),
            ),
        ]
        report = compare.summarize_samples(samples)
        self.assertEqual(report["schemaVersion"], 2)
        self.assertEqual(report["sampleCount"], 2)
        self.assertEqual(report["activeShadowSamples"], 1)
        self.assertEqual(report["presenceMismatches"], 0)

    def test_inactive_sample_without_diagnostics_breaks_overcount_episode(self):
        active_overcount = groups_fixture(
            tracks=[
                shadow_track(1, "confirmed", 1),
                shadow_track(2, "confirmed", 1),
            ],
            diagnostics=shadow_diagnostics(0, 0, 0),
        )
        inactive = groups_fixture("not_group_leader", tracks=[])
        del inactive["shadowFusion"]["diagnostics"]
        samples = [
            compare.parse_sample(
                state_fixture(True, 1),
                active_overcount,
                captured_at="2026-07-27T00:00:00+00:00",
            ),
            compare.parse_sample(
                state_fixture(True, 1),
                inactive,
                captured_at="2026-07-27T00:00:01+00:00",
            ),
            compare.parse_sample(
                state_fixture(True, 1),
                active_overcount,
                captured_at="2026-07-27T00:00:02+00:00",
            ),
        ]
        report = compare.summarize_samples(samples)
        self.assertEqual(report["overcountEpisodes"], 2)
        self.assertEqual(report["longestOvercountEpisodeSamples"], 1)

    def test_summary_reports_presence_and_target_disagreements(self):
        samples = [
            compare.parse_sample(
                state_fixture(True, 2), groups_fixture(tracks=[])
            ),
            compare.parse_sample(
                state_fixture(False, 0), groups_fixture()
            ),
        ]
        report = compare.summarize_samples(samples)
        self.assertEqual(report["presenceMismatches"], 2)
        self.assertEqual(report["targetCountMismatches"], 2)
        self.assertEqual(report["presenceMismatchRate"], 1.0)
        self.assertEqual(report["overcountSamples"], 1)
        self.assertEqual(report["undercountSamples"], 1)

    def test_summary_reports_overcount_episodes_and_reasons(self):
        samples = [
            compare.parse_sample(
                state_fixture(True, 1),
                groups_fixture(
                    tracks=[
                        shadow_track(1, "confirmed", 1, 1000, 2000),
                        shadow_track(2, "confirmed", 1, 2500, 2000),
                    ],
                    diagnostics=shadow_diagnostics(0, 0, 0),
                ),
                captured_at="2026-07-27T00:00:00+00:00",
            ),
            compare.parse_sample(
                state_fixture(True, 1),
                groups_fixture(
                    tracks=[
                        shadow_track(1, "confirmed", 1, 1100, 2000),
                        shadow_track(2, "confirmed", 1, 2400, 2000),
                    ],
                    diagnostics=shadow_diagnostics(0, 0, 0),
                ),
                captured_at="2026-07-27T00:00:01+00:00",
            ),
            compare.parse_sample(
                state_fixture(True, 1),
                groups_fixture(),
                captured_at="2026-07-27T00:00:02+00:00",
            ),
            compare.parse_sample(
                state_fixture(True, 1),
                groups_fixture(
                    tracks=[
                        shadow_track(1, "confirmed", 2),
                        shadow_track(2, "coasting", 1),
                    ],
                    diagnostics=shadow_diagnostics(1, 1, 0),
                ),
                captured_at="2026-07-27T00:00:03+00:00",
            ),
        ]
        report = compare.summarize_samples(samples)
        self.assertEqual(report["overcountSamples"], 3)
        self.assertEqual(report["overcountEpisodes"], 2)
        self.assertEqual(report["longestOvercountEpisodeSamples"], 2)
        self.assertEqual(report["longestOvercountEpisodeSeconds"], 1.0)
        self.assertEqual(
            report["overcountReasonSamples"]["no_pair_candidate"], 2
        )
        self.assertEqual(
            report["overcountReasonSamples"]["lifecycle_overlap"], 1
        )
        self.assertEqual(report["maxShadowTrackSeparationMm"], 1500.0)

    def test_overcount_reason_distinguishes_ambiguity(self):
        sample = compare.parse_sample(
            state_fixture(True, 1),
            groups_fixture(
                tracks=[
                    shadow_track(1, "confirmed", 1),
                    shadow_track(2, "confirmed", 1),
                ],
                diagnostics=shadow_diagnostics(2, 0, 1),
            ),
        )
        self.assertEqual(sample.mismatch_kind, "overcount")
        self.assertEqual(sample.overcount_reason, "ambiguous_pair")

    def test_run_is_read_only_and_writes_optional_jsonl(self):
        client = FakeClient()
        output = Path(self.id().replace(".", "_") + ".jsonl")
        monotonic_values = iter([0.0, 0.3, 0.6])
        try:
            with mock.patch.object(
                compare.time, "monotonic", side_effect=monotonic_values
            ), mock.patch.object(compare.time, "sleep"):
                report = compare.run_comparison(
                    client, 0.5, 0.25, output
                )
            self.assertEqual(report["sampleCount"], 2)
            self.assertEqual(
                client.paths,
                [
                    "/api/state",
                    "/api/platform/groups",
                    "/api/state",
                    "/api/platform/groups",
                ],
            )
            lines = output.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(lines), 2)
            captured = json.loads(lines[0])
            self.assertEqual(captured["schema_version"], 2)
            self.assertEqual(captured["production_targets"][0]["x_mm"], 1200.0)
            self.assertEqual(captured["shadow_tracks"][0]["source_count"], 2)
            self.assertIn("shadow_diagnostics", captured)
        finally:
            output.unlink(missing_ok=True)

    def test_parser_rejects_disconnected_production_state(self):
        state = state_fixture()
        state["connected"] = False
        with self.assertRaisesRegex(ValueError, "not connected"):
            compare.parse_sample(state, groups_fixture())


if __name__ == "__main__":
    unittest.main()

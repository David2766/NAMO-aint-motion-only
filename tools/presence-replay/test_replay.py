#!/usr/bin/env python3
"""Unit tests for the compact presence replay parser."""

from __future__ import annotations

import unittest

import replay


def make_row(static_radar: list[int] | None = None) -> dict[str, object]:
    row: dict[str, object] = {
        "q": 1,
        "t": 1000,
        "p": 0,
        "lx": 0,
        "r": [[0, 0, 0, 0, 0, 0, 0, 0] for _ in range(3)],
        "tg": [[0, 0, 0, 0, 0] for _ in range(3)],
        "sf": [0, 0, 0, 0, 0, 0, 0],
        "f": [0, 0, 0, 0, 0],
        "ex": [0, 0, 0, -1],
        "l": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
        "tr": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    }
    if static_radar is not None:
        row["sr"] = static_radar
    return row


class ReplayStaticRadarTupleTest(unittest.TestCase):
    def test_legacy_eight_item_tuple_defaults_canonical_distance(self) -> None:
        sample = replay.parse_sample(make_row([1, 1, 0, 1, 0, 1850, 0, 67]), 1)

        self.assertTrue(sample.static_radar_presence)
        self.assertEqual(sample.static_radar_still_distance_mm, 1850)
        self.assertEqual(sample.static_radar_detection_distance_mm, 0)

    def test_nine_item_tuple_reads_canonical_distance(self) -> None:
        sample = replay.parse_sample(make_row([1, 1, 0, 1, 0, 1900, 0, 67, 1850]), 1)

        self.assertEqual(sample.static_radar_still_distance_mm, 1900)
        self.assertEqual(sample.static_radar_detection_distance_mm, 1850)

    def test_missing_tuple_keeps_legacy_defaults(self) -> None:
        sample = replay.parse_sample(make_row(), 1)

        self.assertFalse(sample.static_radar_available)
        self.assertEqual(sample.static_radar_detection_distance_mm, 0)

    def test_other_tuple_lengths_are_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "must have 8 or 9 items"):
            replay.parse_sample(make_row([0] * 10), 7)


class ReplayTrackerTargetTupleTest(unittest.TestCase):
    def test_legacy_five_item_tuple_defaults_reduced_zone(self) -> None:
        row = make_row()
        row["tg"] = [[1, 100, 2000, 0, 2002], [0, 0, 0, 0, 0], [0, 0, 0, 0, 0]]

        sample = replay.parse_sample(row, 1)

        self.assertFalse(sample.targets[0].reduced_zone)
        self.assertFalse(sample.targets[0].filter_blocked)

    def test_six_item_tuple_reads_reduced_zone(self) -> None:
        row = make_row()
        row["tg"] = [[1, 100, 2000, 0, 2002, 1], [0, 0, 0, 0, 0, 0], [0, 0, 0, 0, 0, 0]]

        sample = replay.parse_sample(row, 1)

        self.assertTrue(sample.targets[0].reduced_zone)
        self.assertFalse(sample.targets[0].filter_blocked)

    def test_seven_item_tuple_reads_per_target_filter_block(self) -> None:
        row = make_row()
        row["tg"] = [[1, 100, 2000, 0, 2002, 0, 1], [0, 0, 0, 0, 0, 0, 0], [0, 0, 0, 0, 0, 0, 0]]

        sample = replay.parse_sample(row, 1)

        self.assertFalse(sample.targets[0].reduced_zone)
        self.assertTrue(sample.targets[0].filter_blocked)


class ReplayRawTargetTupleTest(unittest.TestCase):
    def test_software_reduced_mode_is_replayed_as_reduced(self) -> None:
        row = make_row()
        row["r"] = [[1, 100, 2000, 0, 2002, 3, 0, 1], [0] * 8, [0] * 8]

        sample = replay.parse_sample(row, 1)

        self.assertEqual(sample.raw_targets[0].filter_mode, 3)
        self.assertTrue(sample.raw_targets[0].reduced_zone)
        self.assertFalse(sample.raw_targets[0].filter_blocked)


if __name__ == "__main__":
    unittest.main()

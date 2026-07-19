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


if __name__ == "__main__":
    unittest.main()

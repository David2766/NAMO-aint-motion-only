"""Measure host-side size and CPU cost of NAMO observation codec candidates."""

from __future__ import annotations

import argparse
import copy
import json
import platform
import statistics
import sys
import time
from typing import Any

from codec import available_codecs
from protocol import load_fixtures


def benchmark(iterations: int = 2000) -> dict[str, Any]:
    if iterations < 1:
        raise ValueError("iterations must be positive")
    observations = _scenarios()
    results: dict[str, Any] = {
        "host": {
            "python": platform.python_version(),
            "platform": platform.platform(),
        },
        "iterations": iterations,
        "scenarios": {},
    }
    for scenario_name, observation in observations.items():
        scenario_results: dict[str, Any] = {}
        for codec in available_codecs():
            payload = codec.encode(observation)
            codec.decode(payload)
            encode_samples = _samples(lambda: codec.encode(observation), iterations)
            decode_samples = _samples(lambda: codec.decode(payload), iterations)
            scenario_results[codec.name] = {
                "bytes": len(payload),
                "encodeMedianUs": round(statistics.median(encode_samples) / 1000, 2),
                "decodeMedianUs": round(statistics.median(decode_samples) / 1000, 2),
            }
        results["scenarios"][scenario_name] = scenario_results
    return results


def _samples(function, iterations: int) -> list[int]:
    samples: list[int] = []
    for _ in range(iterations):
        start = time.perf_counter_ns()
        function()
        samples.append(time.perf_counter_ns() - start)
    return samples


def _scenarios() -> dict[str, dict[str, Any]]:
    fixtures = load_fixtures()
    spatial = copy.deepcopy(fixtures["valid"]["observations"][0])
    presence_only = copy.deepcopy(fixtures["valid"]["observations"][1])
    three_tracks = copy.deepcopy(spatial)
    base_track = spatial["tracks"][0]
    three_tracks["tracks"] = []
    for index in range(3):
        track = copy.deepcopy(base_track)
        track["localTrackId"] = index + 1
        track["xMm"] += index * 250
        track["yMm"] += index * 400
        three_tracks["tracks"].append(track)
    return {
        "presence-only": presence_only,
        "one-track": spatial,
        "three-track": three_tracks,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--iterations", type=int, default=2000)
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args()
    result = benchmark(args.iterations)
    if args.as_json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        print(f"host={result['host']['platform']} python={result['host']['python']}")
        print(f"iterations={result['iterations']}")
        for scenario, codecs in result["scenarios"].items():
            print(f"\n[{scenario}]")
            for name, measurement in codecs.items():
                print(
                    f"{name:8} {measurement['bytes']:4} bytes "
                    f"encode={measurement['encodeMedianUs']:8.2f} us "
                    f"decode={measurement['decodeMedianUs']:8.2f} us"
                )
    return 0


if __name__ == "__main__":
    sys.exit(main())

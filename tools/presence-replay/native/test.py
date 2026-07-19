#!/usr/bin/env python3
"""Build and run native PresenceTracker tests."""

from __future__ import annotations

import os
import argparse
import subprocess
import sys
from pathlib import Path

import build


ROOT = Path(__file__).resolve().parents[3]
NATIVE_DIR = Path(__file__).resolve().parent
TEST_CPP = NATIVE_DIR / "tracker_tests.cpp"
DEVICE_CONFIG_CPP = ROOT / "components" / "radar_api_server" / "device_config_cache.cpp"
SOFTWARE_ZONE_EVIDENCE_CPP = ROOT / "components" / "radar_api_server" / "software_zone_evidence.cpp"
PRESENCE_FUSION_CPP = ROOT / "components" / "radar_api_server" / "presence_fusion.cpp"
PRESENCE_REPLAY_LOG_CPP = ROOT / "components" / "radar_api_server" / "presence_replay_log.cpp"
STATE_JSON_BUILDER_CPP = ROOT / "components" / "radar_api_server" / "state_json_builder.cpp"
TEST_INCLUDE_DIR = NATIVE_DIR / "include"
OUT_EXE = NATIVE_DIR / ("tracker_tests.exe" if os.name == "nt" else "tracker_tests")


def test_command(compiler: str, kind: str) -> list[str]:
    msvc_args = [
        "cl",
        "/nologo",
        "/std:c++17",
        "/EHsc",
        "/O2",
        f"/I{TEST_INCLUDE_DIR}",
        f"/I{build.INCLUDE_DIR}",
        str(TEST_CPP),
        str(build.TRACKER_CPP),
        str(PRESENCE_FUSION_CPP),
        str(PRESENCE_REPLAY_LOG_CPP),
        str(STATE_JSON_BUILDER_CPP),
        str(DEVICE_CONFIG_CPP),
        str(SOFTWARE_ZONE_EVIDENCE_CPP),
        f"/Fe:{OUT_EXE}",
    ]

    if kind == "msvc":
        return [compiler, *msvc_args[1:]]

    if kind == "msvc-vsdevcmd":
        batch = NATIVE_DIR / "_test_tracker.cmd"
        compile_command = " ".join(build.cmd_quote(arg) for arg in msvc_args)
        batch.write_text(
            "\n".join(
                [
                    "@echo off",
                    f'call "{compiler}" -arch=x64 -host_arch=x64',
                    "if errorlevel 1 exit /b %errorlevel%",
                    compile_command,
                    "exit /b %errorlevel%",
                    "",
                ]
            ),
            encoding="utf-8",
        )
        return ["cmd", "/c", str(batch)]

    return [
        compiler,
        "-std=c++17",
        "-O2",
        f"-I{TEST_INCLUDE_DIR}",
        f"-I{build.INCLUDE_DIR}",
        str(TEST_CPP),
        str(build.TRACKER_CPP),
        str(PRESENCE_FUSION_CPP),
        str(PRESENCE_REPLAY_LOG_CPP),
        str(STATE_JSON_BUILDER_CPP),
        str(DEVICE_CONFIG_CPP),
        str(SOFTWARE_ZONE_EVIDENCE_CPP),
        "-o",
        str(OUT_EXE),
    ]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check-compiler",
        action="store_true",
        help="Only check whether a supported host C++ compiler is available.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    compiler = build.find_compiler()
    if compiler is None:
        if args.check_compiler:
            print("tracker_tests: skipped (no host C++ compiler found)")
            return 2
        print(
            "error: no host C++ compiler found. Install Visual Studio Build Tools, MinGW g++, or clang++.",
            file=sys.stderr,
        )
        return 1

    compiler_path, kind = compiler
    if args.check_compiler:
        print(f"tracker_tests: compiler found ({compiler_path})")
        return 0
    command = test_command(compiler_path, kind)
    print(" ".join(command))
    completed = subprocess.run(command, cwd=ROOT)
    if completed.returncode != 0:
        return completed.returncode

    return subprocess.run([str(OUT_EXE)], cwd=ROOT).returncode


if __name__ == "__main__":
    raise SystemExit(main())

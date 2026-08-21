#!/usr/bin/env python3
"""Build the host-only NAMO virtual node executable."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
NATIVE_DIR = Path(__file__).resolve().parent / "native"
COMPONENT_DIR = ROOT / "components" / "radar_api_server"
TEST_INCLUDE_DIR = ROOT / "tools" / "presence-replay" / "native" / "include"
OUT_EXE = NATIVE_DIR / ("virtual_node.exe" if os.name == "nt" else "virtual_node")

sys.path.insert(0, str(ROOT / "tools" / "presence-replay" / "native"))
import build as native_build  # noqa: E402


SOURCES = [
    NATIVE_DIR / "virtual_node.cpp",
    COMPONENT_DIR / "participant_registry.cpp",
    COMPONENT_DIR / "participant_readiness.cpp",
    COMPONENT_DIR / "participant_credentials.cpp",
    COMPONENT_DIR / "credential_provisioning.cpp",
    COMPONENT_DIR / "participant_discovery.cpp",
    COMPONENT_DIR / "spatial_transform.cpp",
    COMPONENT_DIR / "group_membership.cpp",
    COMPONENT_DIR / "group_roster.cpp",
    COMPONENT_DIR / "site_configuration.cpp",
    COMPONENT_DIR / "site_directory.cpp",
    COMPONENT_DIR / "site_group_summary.cpp",
    COMPONENT_DIR / "site_heartbeat.cpp",
    COMPONENT_DIR / "site_coordinator.cpp",
    COMPONENT_DIR / "group_coordinator.cpp",
]


def build_command(compiler: str, kind: str) -> list[str]:
    msvc_args = [
        "cl",
        "/nologo",
        "/std:c++17",
        "/EHsc",
        "/O2",
        "/D_CRT_SECURE_NO_WARNINGS",
        f"/I{TEST_INCLUDE_DIR}",
        f"/I{COMPONENT_DIR}",
        *(str(source) for source in SOURCES),
        "ws2_32.lib",
        f"/Fe:{OUT_EXE}",
    ]
    if kind == "msvc":
        return [compiler, *msvc_args[1:]]
    if kind == "msvc-vsdevcmd":
        batch = NATIVE_DIR / "_build_virtual_node.cmd"
        command = " ".join(native_build.cmd_quote(arg) for arg in msvc_args)
        batch.write_text(
            "\n".join(
                [
                    "@echo off",
                    f'call "{compiler}" -arch=x64 -host_arch=x64',
                    "if errorlevel 1 exit /b %errorlevel%",
                    command,
                    "exit /b %errorlevel%",
                    "",
                ]
            ),
            encoding="utf-8",
        )
        return ["cmd", "/c", str(batch)]
    command = [
        compiler,
        "-std=c++17",
        "-O2",
        f"-I{TEST_INCLUDE_DIR}",
        f"-I{COMPONENT_DIR}",
        *(str(source) for source in SOURCES),
        "-o",
        str(OUT_EXE),
    ]
    if os.name == "nt":
        command.append("-lws2_32")
    return command


def main() -> int:
    compiler = native_build.find_compiler()
    if compiler is None:
        print("error: no supported host C++ compiler found", file=sys.stderr)
        return 1
    NATIVE_DIR.mkdir(parents=True, exist_ok=True)
    compiler_path, kind = compiler
    command = build_command(compiler_path, kind)
    print(" ".join(command))
    completed = subprocess.run(command, cwd=ROOT)
    if completed.returncode != 0:
        return completed.returncode
    print(f"built: {OUT_EXE}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

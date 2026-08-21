#!/usr/bin/env python3
"""Back up and repair a split Site configuration from one chosen device."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import re
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Any


REQUIRED_READ_PATHS = (
    "/api/system/status",
    "/api/platform/site-configuration",
    "/api/platform/participants",
    "/api/platform/groups",
)

OPTIONAL_READ_PATHS = (
    "/api/floorplan/status",
    "/api/config",
    "/api/floorplan",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


@dataclass(frozen=True)
class DeviceSnapshot:
    base_url: str
    node_id: str
    site: dict[str, Any]
    payloads: dict[str, Any]


class DeviceClient:
    def __init__(self, base_url: str, timeout: float = 5.0) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout

    def request(self, path: str, data: bytes | None = None) -> bytes:
        request = urllib.request.Request(
            f"{self.base_url}{path}",
            data=data,
            headers={
                "Cache-Control": "no-cache",
                "Content-Type": "application/x-www-form-urlencoded",
            },
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            detail = error.read().decode("utf-8", errors="replace")
            raise RuntimeError(
                f"{self.base_url}{path} returned HTTP {error.code}: {detail}"
            ) from error

    def get_json(self, path: str) -> dict[str, Any]:
        return json.loads(self.request(path))

    def post_form(self, path: str, fields: dict[str, str]) -> dict[str, Any]:
        data = urllib.parse.urlencode(fields).encode("ascii")
        return json.loads(self.request(path, data=data))

    def snapshot(self) -> DeviceSnapshot:
        payloads = {path: self.get_json(path) for path in REQUIRED_READ_PATHS}
        for path in OPTIONAL_READ_PATHS:
            try:
                payloads[path] = self.get_json(path)
            except RuntimeError as error:
                payloads[path] = {"ok": False, "backupError": str(error)}
        system = payloads["/api/system/status"]
        site = payloads["/api/platform/site-configuration"]
        node_id = str(system.get("platform", {}).get("nodeId", ""))
        return DeviceSnapshot(self.base_url, node_id, site, payloads)


def active_member_ids(site: dict[str, Any]) -> set[str]:
    return {
        str(member.get("participantId", ""))
        for member in site.get("members", [])
        if member.get("membership") == "active" and member.get("participantId")
    }


def manifest_key(site: dict[str, Any]) -> tuple[Any, ...]:
    floorplan = site.get("floorplan", {})
    return (
        floorplan.get("floorplanId"),
        floorplan.get("geometryVersion"),
        floorplan.get("configHash"),
        floorplan.get("imageHash"),
    )


def configured_groups(snapshot: DeviceSnapshot) -> dict[str, tuple[Any, ...]]:
    groups = snapshot.payloads.get("/api/platform/groups", {})
    return {
        str(group.get("groupId", "")): (
            group.get("leaderNodeId"),
            group.get("term"),
            group.get("health"),
        )
        for group in groups.get("groups", [])
        if group.get("configured") is True and group.get("groupId")
    }


def coordinators_converged(source: DeviceSnapshot,
                           peer: DeviceSnapshot) -> bool:
    source_groups = source.payloads.get("/api/platform/groups", {})
    peer_groups = peer.payloads.get("/api/platform/groups", {})
    source_site = source_groups.get("site", {})
    peer_site = peer_groups.get("site", {})
    source_configured = configured_groups(source)
    peer_configured = configured_groups(peer)
    return (
        bool(source_site.get("leaderNodeId"))
        and source_site.get("leaderNodeId") == peer_site.get("leaderNodeId")
        and source_site.get("term") == peer_site.get("term")
        and source_site.get("health") != "configuration_conflict"
        and peer_site.get("health") != "configuration_conflict"
        and source_configured == peer_configured
        and all(group[0] and group[2] != "configuration_conflict"
                for group in source_configured.values())
    )


def validate_repair_pair(source: DeviceSnapshot, peer: DeviceSnapshot) -> None:
    require(source.node_id and peer.node_id and source.node_id != peer.node_id,
            "source and peer must be two distinct NAMO devices")
    require(source.site.get("schemaVersion", 0) >= 2 and
            peer.site.get("schemaVersion", 0) >= 2,
            "both devices need firmware with the fenced Site configuration API")
    require(source.site.get("siteId") == peer.site.get("siteId"),
            "source and peer do not belong to the same Site")
    require(source.site.get("configurationConsistent") is False and
            peer.site.get("configurationConsistent") is False,
            "both devices must report the configuration conflict")
    require(
        source.site.get("configurationDigest") !=
            peer.site.get("configurationDigest")
        or source.site.get("configurationOwnerNodeId") !=
            peer.site.get("configurationOwnerNodeId")
        or source.site.get("authorityEpoch") != peer.site.get("authorityEpoch")
        or source.site.get("generation") != peer.site.get("generation"),
        "both snapshots already describe the same canonical configuration; wait for heartbeat convergence",
    )
    require(source.site.get("localOwner") is True and
            source.site.get("configurationOwnerNodeId") == source.node_id,
            "the selected source must currently own its local configuration")
    require(active_member_ids(source.site) == active_member_ids(peer.site),
            "active Site rosters differ; automatic two-device repair is unsafe")
    digest = str(source.site.get("configurationDigest", ""))
    require(re.fullmatch(r"[0-9a-fA-F]{16}", digest) is not None,
            "the source did not expose a valid configuration digest")


def converged(source: DeviceSnapshot, peer: DeviceSnapshot) -> bool:
    source_site = source.site
    peer_site = peer.site
    return (
        source_site.get("configurationConsistent") is True
        and peer_site.get("configurationConsistent") is True
        and source_site.get("siteId") == peer_site.get("siteId")
        and source_site.get("authorityEpoch") == peer_site.get("authorityEpoch")
        and source_site.get("generation") == peer_site.get("generation")
        and source_site.get("configurationDigest") ==
            peer_site.get("configurationDigest")
        and source_site.get("configurationOwnerNodeId") ==
            peer_site.get("configurationOwnerNodeId")
        and manifest_key(source_site) == manifest_key(peer_site)
        and coordinators_converged(source, peer)
    )


def backup_device(client: DeviceClient, snapshot: DeviceSnapshot,
                  directory: pathlib.Path) -> None:
    safe_node_id = re.sub(r"[^A-Za-z0-9_.-]", "_", snapshot.node_id)
    device_directory = directory / safe_node_id
    device_directory.mkdir(parents=True, exist_ok=True)
    for path, payload in snapshot.payloads.items():
        name = path.strip("/").replace("/", "-") or "root"
        (device_directory / f"{name}.json").write_text(
            json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
    status = snapshot.payloads.get("/api/floorplan/status", {})
    if status.get("hasImage") is True:
        (device_directory / "floorplan-image.bin").write_bytes(
            client.request("/api/floorplan/image")
        )


def wait_for_convergence(source: DeviceClient, peer: DeviceClient,
                         timeout_seconds: float) -> tuple[DeviceSnapshot,
                                                          DeviceSnapshot]:
    deadline = time.monotonic() + timeout_seconds
    last_error = "no response"
    while time.monotonic() < deadline:
        try:
            source_snapshot = source.snapshot()
            peer_snapshot = peer.snapshot()
            if converged(source_snapshot, peer_snapshot):
                return source_snapshot, peer_snapshot
            last_error = (
                f"source={source_snapshot.site.get('authorityEpoch')}/"
                f"{source_snapshot.site.get('generation')}/"
                f"{source_snapshot.site.get('configurationDigest')} "
                f"peer={peer_snapshot.site.get('authorityEpoch')}/"
                f"{peer_snapshot.site.get('generation')}/"
                f"{peer_snapshot.site.get('configurationDigest')}"
            )
        except (OSError, ValueError, RuntimeError) as error:
            last_error = str(error)
        time.sleep(0.5)
    raise RuntimeError(f"Site configuration convergence timed out: {last_error}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True,
                        help="Device whose Site configuration and floorplan must win.")
    parser.add_argument("--peer", required=True,
                        help="Conflicting peer that must receive the selected source.")
    parser.add_argument("--confirm", action="store_true",
                        help="Apply the repair after both devices are backed up.")
    parser.add_argument("--timeout-seconds", type=float, default=90.0)
    parser.add_argument("--backup-dir", default="site-conflict-backups")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    source = DeviceClient(args.source)
    peer = DeviceClient(args.peer)
    source_snapshot = source.snapshot()
    peer_snapshot = peer.snapshot()
    validate_repair_pair(source_snapshot, peer_snapshot)

    timestamp = dt.datetime.now().astimezone().strftime("%Y%m%d-%H%M%S")
    backup_directory = pathlib.Path(args.backup_dir) / timestamp
    backup_device(source, source_snapshot, backup_directory)
    backup_device(peer, peer_snapshot, backup_directory)
    print(f"site_configuration_conflict_repair: backups={backup_directory}")
    print(
        "site_configuration_conflict_repair: selected "
        f"source={source_snapshot.node_id} "
        f"digest={source_snapshot.site.get('configurationDigest')} "
        f"floorplan={manifest_key(source_snapshot.site)}"
    )
    if not args.confirm:
        print("site_configuration_conflict_repair: dry run; add --confirm to apply")
        return 0

    source.post_form(
        "/api/platform/site-configuration",
        {
            "action": "resolveConflict",
            "confirm": "1",
            "expectedDigest": str(source_snapshot.site["configurationDigest"]),
        },
    )
    repaired_source, repaired_peer = wait_for_convergence(
        source, peer, args.timeout_seconds
    )
    print(
        "site_configuration_conflict_repair: converged "
        f"owner={repaired_source.site.get('configurationOwnerNodeId')} "
        f"epoch={repaired_source.site.get('authorityEpoch')} "
        f"generation={repaired_source.site.get('generation')} "
        f"digest={repaired_source.site.get('configurationDigest')}"
    )
    require(converged(repaired_source, repaired_peer),
            "post-repair convergence check failed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError) as error:
        raise SystemExit(f"site_configuration_conflict_repair: error: {error}")

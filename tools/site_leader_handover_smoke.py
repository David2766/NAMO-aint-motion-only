#!/usr/bin/env python3
"""Validate a two-node Site and exercise leader or configuration-owner handover."""

from __future__ import annotations

import argparse
import json
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any, Callable


@dataclass(frozen=True)
class DeviceSnapshot:
    base_url: str
    node_id: str
    site_id: str
    participants: dict[str, Any]
    groups: dict[str, Any]
    configuration: dict[str, Any]


class DeviceClient:
    def __init__(self, base_url: str, timeout: float = 3.0) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout

    def get(self, path: str) -> dict[str, Any]:
        request = urllib.request.Request(
            f"{self.base_url}{path}", headers={"Cache-Control": "no-cache"}
        )
        with urllib.request.urlopen(request, timeout=self.timeout) as response:
            return json.load(response)

    def snapshot(self) -> DeviceSnapshot:
        status = self.get("/api/system/status")
        return DeviceSnapshot(
            base_url=self.base_url,
            node_id=str(status.get("platform", {}).get("nodeId", "")),
            site_id=str(status.get("platform", {}).get("siteId", "")),
            participants=self.get("/api/platform/participants"),
            groups=self.get("/api/platform/groups"),
            configuration=self.get("/api/platform/site-configuration"),
        )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def validate_active_pair(first: DeviceSnapshot, second: DeviceSnapshot) -> str:
    require(first.node_id and second.node_id and first.node_id != second.node_id,
            "devices must expose two distinct Node IDs")
    require(first.site_id and first.site_id == second.site_id,
            "devices must belong to the same Site")
    require(first.configuration.get("configurationConsistent") is not False and
            second.configuration.get("configurationConsistent") is not False,
            "both Nodes must agree on one fenced Site configuration")
    _validate_peer(first, second.node_id)
    _validate_peer(second, first.node_id)

    first_site = first.groups.get("site", {})
    second_site = second.groups.get("site", {})
    leader = str(first_site.get("leaderNodeId", ""))
    require(leader in (first.node_id, second.node_id),
            "Site leader must be one of the two active Nodes")
    require(leader == second_site.get("leaderNodeId"),
            "both Nodes must agree on the Site leader")
    require(first_site.get("term") == second_site.get("term"),
            "both Nodes must agree on the Site term")
    require(first_site.get("configurationReady") is True and
            second_site.get("configurationReady") is True,
            "both Nodes need a committed Site configuration")
    require(first_site.get("health") == "healthy" and
            second_site.get("health") == "healthy",
            "the two-node Site must be healthy before handover")

    first_groups = _configured_groups(first.groups)
    second_groups = _configured_groups(second.groups)
    eligible_nodes = _leader_eligible_nodes(first.configuration)
    require(
        eligible_nodes == _leader_eligible_nodes(second.configuration),
        "both Nodes must agree on Leader eligibility",
    )
    require(first_groups and first_groups.keys() == second_groups.keys(),
            "both Nodes must expose the same configured fusion groups")
    for group_id, first_group in first_groups.items():
        second_group = second_groups[group_id]
        group_leader = str(first_group.get("leaderNodeId", ""))
        require(group_leader == second_group.get("leaderNodeId") and
                first_group.get("term") == second_group.get("term"),
                f"group {group_id} leader or term differs between Nodes")
        require(first_group.get("health") == "healthy" and
                second_group.get("health") == "healthy",
                f"group {group_id} is not healthy")
        require(group_leader in eligible_nodes,
                f"group {group_id} leader is not eligible")
        if len(eligible_nodes) > 1:
            require(group_leader != leader,
                    f"group {group_id} must not share the Site leader while another candidate is available")
    return leader


def wait_for_takeover(
    follower: DeviceClient,
    old_leader: str,
    timeout_seconds: float,
    sleep: Callable[[float], None] = time.sleep,
) -> DeviceSnapshot:
    deadline = time.monotonic() + timeout_seconds
    last_error = "no coordinator response"
    while time.monotonic() < deadline:
        try:
            snapshot = follower.snapshot()
            site = snapshot.groups.get("site", {})
            if site.get("leaderNodeId") == snapshot.node_id and site.get("localLeader") is True:
                require(site.get("health") == "degraded",
                        "takeover must retain the missing Node as degraded, not erase it")
                for group_id, group in _configured_groups(snapshot.groups).items():
                    require(
                        group.get("leaderNodeId") == snapshot.node_id
                        and group.get("localLeader") is True,
                        f"group {group_id} must fall back to the remaining Node",
                    )
                return snapshot
            last_error = f"leader={site.get('leaderNodeId')} local={site.get('localLeader')}"
        except (OSError, urllib.error.URLError, ValueError, RuntimeError) as cause:
            last_error = str(cause)
        sleep(0.5)
    raise RuntimeError(f"leader handover timed out: {last_error}")


def wait_for_rejoin(
    first: DeviceClient,
    second: DeviceClient,
    timeout_seconds: float,
    sleep: Callable[[float], None] = time.sleep,
) -> tuple[DeviceSnapshot, DeviceSnapshot]:
    deadline = time.monotonic() + timeout_seconds
    last_error = "devices not reachable"
    while time.monotonic() < deadline:
        try:
            first_snapshot = first.snapshot()
            second_snapshot = second.snapshot()
            validate_active_pair(first_snapshot, second_snapshot)
            return first_snapshot, second_snapshot
        except (OSError, urllib.error.URLError, ValueError, RuntimeError) as cause:
            last_error = str(cause)
        sleep(0.75)
    raise RuntimeError(f"leader rejoin timed out: {last_error}")


def expected_configuration_successor(configuration: dict[str, Any]) -> str:
    owner = str(configuration.get("configurationOwnerNodeId", ""))
    candidates = sorted(
        str(member.get("participantId", ""))
        for member in configuration.get("members", [])
        if member.get("membership") == "active"
        and member.get("participantType") == "namo_node"
        and member.get("leaderEligible") is True
        and member.get("participantId") != owner
    )
    require(bool(candidates), "no automatic configuration-owner successor is eligible")
    return candidates[0]


def wait_for_configuration_owner(
    device: DeviceClient,
    expected_owner: str,
    timeout_seconds: float,
    sleep: Callable[[float], None] = time.sleep,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_seconds
    last_error = "configuration owner did not change"
    while time.monotonic() < deadline:
        try:
            configuration = device.get("/api/platform/site-configuration")
            owner = str(configuration.get("configurationOwnerNodeId", ""))
            if owner == expected_owner and configuration.get("localOwner") is True:
                return configuration
            handover = configuration.get("automaticHandover", {})
            last_error = (
                f"owner={owner} state={handover.get('state')} "
                f"condition={handover.get('condition')}"
            )
        except (OSError, urllib.error.URLError, ValueError) as cause:
            last_error = str(cause)
        sleep(0.5)
    raise RuntimeError(f"configuration-owner handover timed out: {last_error}")


def wait_for_configuration_convergence(
    first: DeviceClient,
    second: DeviceClient,
    expected_owner: str,
    timeout_seconds: float,
    sleep: Callable[[float], None] = time.sleep,
) -> tuple[DeviceSnapshot, DeviceSnapshot]:
    deadline = time.monotonic() + timeout_seconds
    last_error = "configuration did not converge"
    while time.monotonic() < deadline:
        try:
            first_snapshot = first.snapshot()
            second_snapshot = second.snapshot()
            first_configuration = first_snapshot.configuration
            second_configuration = second_snapshot.configuration
            fields = (
                "siteId",
                "authorityEpoch",
                "generation",
                "configurationDigest",
                "rosterRevision",
                "configurationOwnerNodeId",
            )
            if (
                first_configuration.get("configurationOwnerNodeId")
                == expected_owner
                and all(
                    first_configuration.get(field) == second_configuration.get(field)
                    for field in fields
                )
                and first_configuration.get("floorplan")
                == second_configuration.get("floorplan")
                and first_configuration.get("configurationConsistent") is not False
                and second_configuration.get("configurationConsistent") is not False
            ):
                validate_active_pair(first_snapshot, second_snapshot)
                return first_snapshot, second_snapshot
            last_error = (
                f"first_owner={first_configuration.get('configurationOwnerNodeId')} "
                f"second_owner={second_configuration.get('configurationOwnerNodeId')} "
                f"first_generation={first_configuration.get('generation')} "
                f"second_generation={second_configuration.get('generation')}"
            )
        except (OSError, urllib.error.URLError, ValueError, RuntimeError) as cause:
            last_error = str(cause)
        sleep(0.75)
    raise RuntimeError(f"configuration-owner rejoin timed out: {last_error}")


def _validate_peer(local: DeviceSnapshot, peer_id: str) -> None:
    participant = next(
        (item for item in local.participants.get("participants", [])
         if item.get("participantId") == peer_id),
        None,
    )
    require(participant is not None, f"{local.node_id} has no record for {peer_id}")
    require(participant.get("membership") == "active",
            f"{peer_id} is not active on {local.node_id}")
    require(participant.get("readiness", {}).get("ready") is True,
            f"{peer_id} is not ready on {local.node_id}")
    credential = participant.get("credential", {})
    require(credential.get("provisioned") is True and credential.get("enabled") is True,
            f"{peer_id} credential is not enabled on {local.node_id}")
    require(participant.get("endpoint", {}).get("online") is True,
            f"{peer_id} heartbeat is not online on {local.node_id}")


def _configured_groups(payload: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {
        str(group["groupId"]): group
        for group in payload.get("groups", [])
        if group.get("configured") is True and group.get("groupId")
    }


def _leader_eligible_nodes(configuration: dict[str, Any]) -> set[str]:
    return {
        str(member.get("participantId", ""))
        for member in configuration.get("members", [])
        if member.get("membership") == "active"
        and member.get("participantType") == "namo_node"
        and member.get("leaderEligible") is True
        and member.get("participantId")
    }


def validate_handover_eligibility(
    first_configuration: dict[str, Any],
    second_configuration: dict[str, Any],
    candidate_id: str,
) -> None:
    for configuration in (first_configuration, second_configuration):
        member = next(
            (
                item
                for item in configuration.get("members", [])
                if item.get("participantId") == candidate_id
            ),
            None,
        )
        require(
            member is not None and member.get("membership") == "active",
            f"handover target {candidate_id} is not active in both Site rosters",
        )
        require(
            member.get("leaderEligible") is True,
            f"handover target {candidate_id} is not Leader-eligible in both Site rosters",
        )


def validate_fusion_snapshot_state(
    leader: DeviceSnapshot,
    follower: DeviceSnapshot,
    leader_state: dict[str, Any],
    follower_state: dict[str, Any],
) -> None:
    leader_groups = _configured_groups(leader.groups)
    follower_groups = _configured_groups(follower.groups)
    require(len(leader_groups) == 1 and leader_groups.keys() == follower_groups.keys(),
            "fusion snapshot smoke requires one shared configured Group")
    group_id = next(iter(leader_groups))
    group_leader_id = str(leader_groups[group_id].get("leaderNodeId", ""))
    require(group_leader_id == leader.node_id,
            "leader snapshot was read from the wrong device")
    require(follower_groups[group_id].get("leaderNodeId") == group_leader_id,
            "follower does not agree on the Group leader")

    site_view = (
        leader if leader.groups.get("site", {}).get("localLeader") is True
        else follower
    )
    site_route = next(
        (
            item
            for item in site_view.groups.get("siteGroups", [])
            if item.get("groupId") == group_id
        ),
        None,
    )
    require(site_route is not None,
            "Site leader does not expose the Group automation route")
    require(bool(site_route.get("spaceId")),
            "Group automation route is missing its stable Space identity")
    route = site_route.get("route", {})
    require(route.get("schemaVersion") == 1 and
            route.get("available") is True and
            route.get("statePath") == "/api/state" and
            int(route.get("port", 0)) == 80,
            "Group automation route is unavailable or incomplete")
    if group_leader_id == site_view.node_id:
        require(route.get("local") is True and route.get("address") is None,
                "local Group route must resolve against the Site leader origin")
    else:
        require(route.get("local") is False and
                bool(route.get("address")),
                "remote Group route is missing its authenticated address")

    leader_fusion = leader_state.get("fusion", {})
    follower_fusion = follower_state.get("fusion", {})
    require(leader_fusion.get("buildEnabled") is True and
            leader_fusion.get("authoritative") is True,
            "Group leader does not expose authoritative fusion state")
    require(follower_fusion.get("buildEnabled") is True and
            follower_fusion.get("authoritative") is True,
            "follower local /api/state did not adopt the Fusion Snapshot")
    require(follower_fusion.get("state") == "active",
            "follower Fusion Snapshot state is not active")

    leader_automation = leader_state.get("automation", {})
    follower_automation = follower_state.get("automation", {})
    for label, automation in (
        ("Group leader", leader_automation),
        ("follower", follower_automation),
    ):
        require(automation.get("schemaVersion") == 1 and
                automation.get("complete") is True,
                f"{label} does not expose a complete automation snapshot")
        require(automation.get("health") == "healthy" and
                automation.get("available") is True,
                f"{label} automation snapshot is not healthy")
        require(automation.get("siteId") == leader.site_id and
                automation.get("spaceId") == site_route.get("spaceId") and
                automation.get("groupId") == group_id and
                automation.get("leaderNodeId") == group_leader_id,
                f"{label} automation authority identity does not match the Group")
        require(bool(automation.get("spaceId")) and
                bool(automation.get("bootId")) and
                int(automation.get("groupTerm", 0)) > 0 and
                int(automation.get("sequence", 0)) > 0 and
                int(automation.get("sourceSequence", 0)) > 0,
                f"{label} automation ordering fence is incomplete")
        require(isinstance(automation.get("zones"), list),
                f"{label} automation snapshot is missing its complete Zone list")
    require(leader_automation.get("source") == "local_group_leader",
            "Group leader automation source is not local")
    require(follower_automation.get("source") == "group_leader_snapshot",
            "follower automation source is not the admitted Fusion Snapshot")

    diagnostics = (
        follower.participants.get("shadowTransport", {})
        .get("fusionSnapshot", {})
    )
    require(int(diagnostics.get("admitted", 0)) > 0,
            "follower has not admitted a Fusion Snapshot")
    require(diagnostics.get("source") == group_leader_id,
            "follower snapshot source is not the current Group leader")
    require(int(diagnostics.get("rejected", 0)) == 0,
            "follower rejected one or more Fusion Snapshots")


def wait_for_fusion_snapshot(
    first: DeviceClient,
    second: DeviceClient,
    timeout_seconds: float,
    sleep: Callable[[float], None] = time.sleep,
) -> tuple[DeviceSnapshot, DeviceSnapshot]:
    deadline = time.monotonic() + timeout_seconds
    last_error = "fusion snapshot not available"
    while time.monotonic() < deadline:
        try:
            first_snapshot = first.snapshot()
            second_snapshot = second.snapshot()
            validate_active_pair(first_snapshot, second_snapshot)
            groups = _configured_groups(first_snapshot.groups)
            require(len(groups) == 1,
                    "fusion snapshot smoke requires one configured Group")
            group_leader_id = str(next(iter(groups.values())).get("leaderNodeId", ""))
            leader_client = first if first_snapshot.node_id == group_leader_id else second
            follower_client = second if leader_client is first else first
            leader_snapshot = first_snapshot if leader_client is first else second_snapshot
            follower_snapshot = second_snapshot if leader_client is first else first_snapshot
            validate_fusion_snapshot_state(
                leader_snapshot,
                follower_snapshot,
                leader_client.get("/api/state"),
                follower_client.get("/api/state"),
            )
            return first_snapshot, second_snapshot
        except (OSError, urllib.error.URLError, ValueError, RuntimeError) as cause:
            last_error = str(cause)
        sleep(0.5)
    raise RuntimeError(f"fusion snapshot timed out: {last_error}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device-a", required=True)
    parser.add_argument("--device-b", required=True)
    handover = parser.add_mutually_exclusive_group()
    handover.add_argument(
        "--handover",
        action="store_true",
        help="Prompt for a manual leader power cycle and validate takeover/rejoin.",
    )
    handover.add_argument(
        "--configuration-owner-handover",
        action="store_true",
        help="Power-cycle the current configuration owner and validate automatic ownership convergence.",
    )
    parser.add_argument("--timeout-seconds", type=float, default=30.0)
    parser.add_argument(
        "--fusion-snapshot",
        action="store_true",
        help="Verify that the non-computing Node serves the authenticated fusion result from its own /api/state.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    first = DeviceClient(args.device_a)
    second = DeviceClient(args.device_b)
    first_snapshot = first.snapshot()
    second_snapshot = second.snapshot()
    leader_id = validate_active_pair(first_snapshot, second_snapshot)
    print(f"site_leader_handover_smoke: active pair healthy, leader={leader_id}")
    if args.fusion_snapshot:
        wait_for_fusion_snapshot(first, second, args.timeout_seconds)
        print("site_leader_handover_smoke: fusion snapshot passed")
    if args.configuration_owner_handover:
        first_owner = str(
            first_snapshot.configuration.get("configurationOwnerNodeId", "")
        )
        second_owner = str(
            second_snapshot.configuration.get("configurationOwnerNodeId", "")
        )
        require(first_owner and first_owner == second_owner,
                "both Nodes must agree on the configuration owner")
        expected_successor = expected_configuration_successor(
            first_snapshot.configuration
        )
        require(
            expected_successor
            == expected_configuration_successor(second_snapshot.configuration),
            "both Nodes must derive the same automatic successor",
        )
        owner = first if first_snapshot.node_id == first_owner else second
        successor = first if first_snapshot.node_id == expected_successor else second
        require(owner is not successor,
                "configuration owner and successor must be different devices")
        input(
            f"Power off configuration owner {first_owner}, then press Enter. "
            "No API reboot will be sent. "
        )
        promoted = wait_for_configuration_owner(
            successor, expected_successor, args.timeout_seconds
        )
        print(
            "site_leader_handover_smoke: configuration-owner takeover passed, "
            f"owner={promoted.get('configurationOwnerNodeId')}"
        )
        input(f"Power {first_owner} back on, then press Enter. ")
        converged = wait_for_configuration_convergence(
            first, second, expected_successor, args.timeout_seconds
        )
        generation = converged[0].configuration.get("generation")
        authority_epoch = converged[0].configuration.get("authorityEpoch")
        print(
            "site_leader_handover_smoke: configuration-owner rejoin passed, "
            f"owner={expected_successor}, epoch={authority_epoch}, "
            f"generation={generation}"
        )
        return 0
    if not args.handover:
        return 0

    leader = first if first_snapshot.node_id == leader_id else second
    follower = second if leader is first else first
    follower_snapshot = second_snapshot if leader is first else first_snapshot
    validate_handover_eligibility(
        first.get("/api/platform/site-configuration"),
        second.get("/api/platform/site-configuration"),
        follower_snapshot.node_id,
    )
    input(f"Power off leader {leader_id}, then press Enter. No API reboot will be sent. ")
    takeover = wait_for_takeover(follower, leader_id, args.timeout_seconds)
    print(f"site_leader_handover_smoke: takeover passed, leader={takeover.node_id}")
    input(f"Power {leader_id} back on, then press Enter. ")
    rejoined = wait_for_rejoin(first, second, args.timeout_seconds)
    final_leader = rejoined[0].groups.get("site", {}).get("leaderNodeId")
    print(f"site_leader_handover_smoke: rejoin passed, leader={final_leader}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

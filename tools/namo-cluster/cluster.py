#!/usr/bin/env python3
"""Run 2-7 host NAMO processes with real UDP/HTTP failure injection."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Callable


ROOT = Path(__file__).resolve().parents[2]
TOOL_DIR = Path(__file__).resolve().parent
NODE_EXE = TOOL_DIR / "native" / ("virtual_node.exe" if os.name == "nt" else "virtual_node")
BUILD_SCRIPT = TOOL_DIR / "build.py"
DEFAULT_RESULTS = ROOT / "test-result" / "namo-cluster"


class ClusterError(RuntimeError):
    pass


@dataclass
class NodeProcess:
    index: int
    node_id: str
    state_dir: Path
    log_path: Path
    process: subprocess.Popen[bytes] | None = None
    log_file: Any = None


class VirtualCluster:
    def __init__(
        self,
        *,
        node_count: int,
        output_dir: Path,
        udp_base: int,
        http_base: int,
        time_scale: int,
    ) -> None:
        if not 2 <= node_count <= 7:
            raise ClusterError("node count must be between 2 and 7")
        self.node_count = node_count
        self.output_dir = output_dir
        self.udp_base = udp_base
        self.http_base = http_base
        self.time_scale = time_scale
        self.site_id = f"site-virtual-{node_count}"
        self.node_ids = [f"namo-node-{index:02d}" for index in range(node_count)]
        self.owner_id = self.node_ids[0]
        self.nodes = [
            NodeProcess(
                index=index,
                node_id=node_id,
                state_dir=output_dir / "state" / node_id,
                log_path=output_dir / "logs" / f"{node_id}.log",
            )
            for index, node_id in enumerate(self.node_ids)
        ]
        self.events_path = output_dir / "events.jsonl"
        self.output_dir.mkdir(parents=True, exist_ok=True)
        (self.output_dir / "logs").mkdir(parents=True, exist_ok=True)
        self._started_at = time.monotonic()

    def _event(self, event: str, **fields: Any) -> None:
        record = {
            "at": datetime.now().astimezone().isoformat(),
            "elapsedSeconds": round(time.monotonic() - self._started_at, 3),
            "event": event,
            **fields,
        }
        with self.events_path.open("a", encoding="utf-8") as output:
            output.write(json.dumps(record, ensure_ascii=True, sort_keys=True) + "\n")

    def start_all(self) -> None:
        for node in self.nodes:
            self.start(node.index)
        self.wait_until(
            lambda: len(self.states()) == self.node_count,
            timeout=10,
            description="all HTTP endpoints",
        )

    def start(self, index: int) -> None:
        node = self.nodes[index]
        if node.process is not None and node.process.poll() is None:
            raise ClusterError(f"{node.node_id} is already running")
        node.state_dir.mkdir(parents=True, exist_ok=True)
        node.log_file = node.log_path.open("ab")
        command = [
            str(NODE_EXE),
            "--node-id",
            node.node_id,
            "--site-id",
            self.site_id,
            "--owner-id",
            self.owner_id,
            "--members",
            ",".join(self.node_ids),
            "--state-dir",
            str(node.state_dir),
            "--udp-base",
            str(self.udp_base),
            "--http-base",
            str(self.http_base),
            "--time-scale",
            str(self.time_scale),
        ]
        node.process = subprocess.Popen(
            command,
            cwd=ROOT,
            stdout=node.log_file,
            stderr=subprocess.STDOUT,
        )
        self._event("power_on", node=node.node_id, pid=node.process.pid)

    def stop(self, index: int, *, graceful: bool = False) -> None:
        node = self.nodes[index]
        if node.process is None or node.process.poll() is not None:
            return
        self._event("power_off", node=node.node_id, graceful=graceful)
        if graceful:
            try:
                self.post(index, "/control/stop")
            except Exception:
                pass
        try:
            node.process.wait(timeout=2 if graceful else 0.1)
        except subprocess.TimeoutExpired:
            node.process.terminate()
            try:
                node.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                node.process.kill()
                node.process.wait(timeout=2)
        if node.log_file is not None:
            node.log_file.close()
            node.log_file = None

    def close(self) -> None:
        for node in self.nodes:
            self.stop(node.index, graceful=True)

    def running_indices(self) -> list[int]:
        return [
            node.index
            for node in self.nodes
            if node.process is not None and node.process.poll() is None
        ]

    def url(self, index: int, path: str) -> str:
        return f"http://127.0.0.1:{self.http_base + index}{path}"

    def request(self, index: int, path: str, *, method: str = "GET") -> dict[str, Any]:
        last_error: Exception | None = None
        for attempt in range(3):
            request = urllib.request.Request(self.url(index, path), method=method)
            try:
                with urllib.request.urlopen(request, timeout=1) as response:
                    return json.load(response)
            except (OSError, urllib.error.URLError, json.JSONDecodeError) as error:
                last_error = error
                if attempt < 2:
                    time.sleep(0.05 * (attempt + 1))
        raise ClusterError(
            f"{method} {self.node_ids[index]}{path} failed after 3 attempts: {last_error}"
        )

    def post(self, index: int, path: str) -> dict[str, Any]:
        return self.request(index, path, method="POST")

    def state(self, index: int) -> dict[str, Any]:
        return self.request(index, "/state")

    def states(self, indices: list[int] | None = None) -> dict[int, dict[str, Any]]:
        result: dict[int, dict[str, Any]] = {}
        for index in indices if indices is not None else self.running_indices():
            try:
                result[index] = self.state(index)
            except (OSError, urllib.error.URLError, json.JSONDecodeError):
                continue
        return result

    def wait_until(
        self,
        predicate: Callable[[], bool],
        *,
        timeout: float,
        description: str,
    ) -> None:
        deadline = time.monotonic() + timeout
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            try:
                if predicate():
                    return
            except Exception as error:
                last_error = error
            time.sleep(0.1)
        suffix = f": {last_error}" if last_error else ""
        raise ClusterError(f"timed out waiting for {description}{suffix}")

    def consensus_ready(self, expected_indices: list[int]) -> bool:
        states = self.states(expected_indices)
        if len(states) != len(expected_indices):
            return False
        expected_online = len(expected_indices)
        quorum = expected_online >= self.node_count // 2 + 1
        owners = {state["ownerNodeId"] for state in states.values()}
        digests = {state["configurationDigest"] for state in states.values()}
        site_leaders = {state["siteLeaderNodeId"] for state in states.values()}
        group_leaders = {state["groupLeaderNodeId"] for state in states.values()}
        if len(owners) != 1 or len(digests) != 1 or len(site_leaders) != 1 or len(group_leaders) != 1:
            return False
        site_leader = next(iter(site_leaders))
        group_leader = next(iter(group_leaders))
        if quorum:
            if not site_leader or not group_leader:
                return False
            if expected_online > 1 and site_leader == group_leader:
                return False
        elif site_leader or group_leader:
            return False
        for state in states.values():
            if state["siteOnline"] != expected_online or state["groupOnline"] != expected_online:
                return False
            if state["siteQuorum"] != quorum or state["groupQuorum"] != quorum:
                return False
            if state["siteExpected"] != self.node_count or state["groupExpected"] != self.node_count:
                return False
        if sum(bool(state["siteLocalLeader"]) for state in states.values()) != (1 if quorum else 0):
            return False
        if sum(bool(state["groupLocalLeader"]) for state in states.values()) != (1 if quorum else 0):
            return False
        return True

    def wait_consensus(self, expected_indices: list[int], *, timeout: float = 12) -> dict[int, dict[str, Any]]:
        self.wait_until(
            lambda: self.consensus_ready(expected_indices),
            timeout=timeout,
            description=f"{len(expected_indices)}/{self.node_count} role consensus",
        )
        states = self.states(expected_indices)
        self._event("consensus", states=states)
        return states

    def set_block(self, receiver: int, sender: int, blocked: bool) -> None:
        self.post(
            receiver,
            f"/control/block?peer={self.node_ids[sender]}&blocked={1 if blocked else 0}",
        )
        self._event(
            "link_block",
            sender=self.node_ids[sender],
            receiver=self.node_ids[receiver],
            blocked=blocked,
        )

    def partition(self, left: list[int], right: list[int], blocked: bool) -> None:
        for receiver in left:
            for sender in right:
                self.set_block(receiver, sender, blocked)
        for receiver in right:
            for sender in left:
                self.set_block(receiver, sender, blocked)

    def set_drop_every(self, every: int) -> None:
        for index in self.running_indices():
            self.post(index, f"/control/drop?every={every}")
        self._event("periodic_drop", every=every)

    def role_index(self, states: dict[int, dict[str, Any]], field: str) -> int:
        participant_id = next(iter(states.values()))[field]
        try:
            return self.node_ids.index(participant_id)
        except ValueError as error:
            raise ClusterError(f"unknown role participant {participant_id!r}") from error

    def assert_partition(self, majority: list[int], minority: list[int]) -> None:
        majority_states = self.states(majority)
        minority_states = self.states(minority)
        if len(majority_states) != len(majority) or len(minority_states) != len(minority):
            raise ClusterError("partition endpoints became unreachable")
        majority_site = {state["siteLeaderNodeId"] for state in majority_states.values()}
        majority_group = {state["groupLeaderNodeId"] for state in majority_states.values()}
        if len(majority_site) != 1 or len(majority_group) != 1 or not next(iter(majority_site)) or not next(iter(majority_group)):
            raise ClusterError("majority partition did not retain authority")
        for state in minority_states.values():
            if state["siteQuorum"] or state["groupQuorum"] or state["siteLocalLeader"] or state["groupLocalLeader"]:
                raise ClusterError("minority partition retained authority")
        self._event("partition_verified", majority=majority_states, minority=minority_states)

    def run_scenarios(self) -> dict[str, Any]:
        scenarios: list[dict[str, Any]] = []

        def passed(name: str, started: float) -> None:
            scenarios.append({"name": name, "passed": True, "seconds": round(time.monotonic() - started, 3)})
            self._event("scenario_pass", name=name)

        started = time.monotonic()
        states = self.wait_consensus(list(range(self.node_count)))
        passed("initial_convergence", started)

        started = time.monotonic()
        site_leader = self.role_index(states, "siteLeaderNodeId")
        original_boot = states[site_leader]["bootId"]
        self.stop(site_leader)
        time.sleep(0.35)
        self.start(site_leader)
        states = self.wait_consensus(list(range(self.node_count)))
        if states[site_leader]["bootId"] == original_boot:
            raise ClusterError("short outage did not create a new boot identity")
        passed("short_site_leader_restart", started)

        started = time.monotonic()
        group_leader = self.role_index(states, "groupLeaderNodeId")
        self.stop(group_leader)
        survivors = [index for index in range(self.node_count) if index != group_leader]
        self.wait_consensus(survivors)
        self.start(group_leader)
        states = self.wait_consensus(list(range(self.node_count)))
        passed("long_group_leader_outage_and_rejoin", started)

        if self.node_count >= 3:
            started = time.monotonic()
            owner = self.role_index(states, "ownerNodeId")
            old_owner = states[owner]["ownerNodeId"]
            self.stop(owner)
            survivors = [index for index in range(self.node_count) if index != owner]

            def owner_changed() -> bool:
                current = self.states(survivors)
                return (
                    len(current) == len(survivors)
                    and len({state["ownerNodeId"] for state in current.values()}) == 1
                    and next(iter(current.values()))["ownerNodeId"] != old_owner
                    and self.consensus_ready(survivors)
                )

            self.wait_until(owner_changed, timeout=15, description="automatic Owner handover")
            self.start(owner)
            states = self.wait_consensus(list(range(self.node_count)), timeout=15)
            if next(iter(states.values()))["ownerNodeId"] == old_owner:
                raise ClusterError("former Owner reclaimed authority after rejoin")
            passed("owner_handover_and_stale_owner_rejoin", started)

            started = time.monotonic()
            quorum = self.node_count // 2 + 1
            majority = list(range(quorum))
            minority = list(range(quorum, self.node_count))
            self.partition(majority, minority, True)
            time.sleep(2.0)
            self.assert_partition(majority, minority)
            self.partition(majority, minority, False)
            states = self.wait_consensus(list(range(self.node_count)), timeout=15)
            passed("network_partition_and_heal", started)

        started = time.monotonic()
        self.set_drop_every(4)
        time.sleep(2.0)
        self.wait_consensus(list(range(self.node_count)))
        self.set_drop_every(0)
        states = self.wait_consensus(list(range(self.node_count)))
        passed("periodic_packet_loss_and_recovery", started)

        return {
            "passed": True,
            "nodeCount": self.node_count,
            "siteId": self.site_id,
            "seconds": round(time.monotonic() - self._started_at, 3),
            "scenarios": scenarios,
            "finalStates": states,
        }


def build_node() -> None:
    completed = subprocess.run([sys.executable, str(BUILD_SCRIPT)], cwd=ROOT)
    if completed.returncode != 0:
        raise ClusterError(f"virtual node build failed with exit code {completed.returncode}")


def timestamp() -> str:
    return datetime.now().strftime("%Y%m%d-%H%M%S")


def run_test(args: argparse.Namespace, node_count: int, run_index: int = 0) -> dict[str, Any]:
    output = Path(args.output) if args.output else DEFAULT_RESULTS / timestamp()
    if args.matrix:
        output = output / f"{node_count}-nodes"
    if output.exists() and not args.reuse_state:
        shutil.rmtree(output)
    cluster = VirtualCluster(
        node_count=node_count,
        output_dir=output,
        udp_base=args.udp_base + run_index * 100,
        http_base=args.http_base + run_index * 100,
        time_scale=args.time_scale,
    )
    try:
        cluster.start_all()
        result = cluster.run_scenarios()
        (output / "report.json").write_text(
            json.dumps(result, indent=2, ensure_ascii=True, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(f"namo-cluster: {node_count} nodes PASS ({result['seconds']}s) output={output}")
        return result
    except Exception as error:
        failure = {
            "passed": False,
            "nodeCount": node_count,
            "error": str(error),
            "states": cluster.states(),
        }
        (output / "report.json").write_text(
            json.dumps(failure, indent=2, ensure_ascii=True, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        cluster._event("failure", error=str(error), states=failure["states"])
        raise
    finally:
        cluster.close()


def run_manual(args: argparse.Namespace) -> None:
    output = Path(args.output) if args.output else DEFAULT_RESULTS / f"manual-{timestamp()}"
    cluster = VirtualCluster(
        node_count=args.nodes,
        output_dir=output,
        udp_base=args.udp_base,
        http_base=args.http_base,
        time_scale=args.time_scale,
    )
    cluster.start_all()
    cluster.wait_consensus(list(range(args.nodes)))
    print(f"namo-cluster: running {args.nodes} nodes")
    for index, node_id in enumerate(cluster.node_ids):
        print(f"  {node_id}: http://127.0.0.1:{args.http_base + index}/state")
    print(f"  output: {output}")
    print("Press Ctrl+C to stop.")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        cluster.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("build", "test", "run"))
    parser.add_argument("--nodes", type=int, default=5)
    parser.add_argument("--matrix", action="store_true", help="test every size from 2 through 7")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--reuse-state", action="store_true")
    parser.add_argument("--output")
    parser.add_argument("--udp-base", type=int, default=47000)
    parser.add_argument("--http-base", type=int, default=48000)
    parser.add_argument("--time-scale", type=int, default=5)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.command == "build":
            build_node()
            return 0
        if not args.no_build or not NODE_EXE.exists():
            build_node()
        if args.command == "run":
            run_manual(args)
            return 0
        counts = list(range(2, 8)) if args.matrix else [args.nodes]
        results = [run_test(args, count, index) for index, count in enumerate(counts)]
        if args.matrix:
            print(f"namo-cluster: matrix PASS ({sum(result['seconds'] for result in results):.1f}s)")
        return 0
    except (ClusterError, OSError, ValueError) as error:
        print(f"namo-cluster: FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

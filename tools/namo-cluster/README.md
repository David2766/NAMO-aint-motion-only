# NAMO host cluster test tool

This tool runs two through seven NAMO nodes as separate host processes. It is
intended for fast role-election and recovery testing before firmware is built
or installed on devices.

## What it exercises

- Production `SiteCoordinator` and `GroupCoordinator` code
- Production Site Configuration codec, validation, A/B store, and handover
- Real localhost UDP traffic between separate node processes
- Real localhost HTTP state and fault-control endpoints
- Process termination and restart with file-backed persistent state
- Two-way packet blocking and periodic packet loss

The automatic scenario suite covers:

1. Initial role convergence
2. Short Site Leader restart
3. Long Group Leader outage and rejoin
4. Configuration Owner handover and stale former Owner rejoin
5. Majority/minority network partition and heal
6. Periodic 25 percent packet loss and recovery

For a two-node cluster, scenarios 4 and 5 are skipped because one surviving
node does not have quorum under the current production contract.

## Run

Build and test one five-node cluster:

```powershell
python tools\namo-cluster\cluster.py test --nodes 5
```

Test every supported size from two through seven:

```powershell
python tools\namo-cluster\cluster.py test --matrix
```

Reuse an existing host build:

```powershell
python tools\namo-cluster\cluster.py test --matrix --no-build
```

Keep a cluster running for manual inspection:

```powershell
python tools\namo-cluster\cluster.py run --nodes 5
```

The `run` command prints each node's `/state` URL. Stop it with `Ctrl+C`.

## Results

Results are written below `test-result/namo-cluster/` by default:

- `report.json`: pass/fail result, scenario timings, and final node states
- `events.jsonl`: ordered power, fault, convergence, and recovery events
- `logs/`: stdout/stderr from each virtual node process
- `state/`: node-specific persistent Site Configuration slots and boot counter

Use `--output <path>` to choose a stable result directory. Existing output is
removed unless `--reuse-state` is supplied.

## Scope boundary

This is a process-level host integration test, not an ESP32 emulator. It uses a
small host-only UDP envelope around production state objects so it does not
exercise ESP-IDF sockets, Wi-Fi behavior, FreeRTOS task stacks, embedded RAM,
the firmware HTTP handlers, transport HMAC, browser pairing, Floorplan transfer,
calibration, or activation. It therefore cannot prove the complete user journey
from the dashboard through persistent commit and production fusion. Keep native
unit tests, browser fixtures, device resource smoke tests, and final multi-device
hardware tests as separate gates.

The native host suite covers seeded 1-7 Node role cases, same-revision candidate
isolation, stale rejoin, and bounded handover retry. Equivalent process-level
candidate-loss and repeated former-Owner sequences should still extend this tool
without pretending it is an embedded-resource, browser, or physical-radio test.

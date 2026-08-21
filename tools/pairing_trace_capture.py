from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import secrets
import socket
import threading
import time
from typing import Any
import urllib.error
import urllib.parse
import urllib.request


SCHEMA_VERSION = 1
MAX_DEVICES = 7
MAX_EVENT_BYTES = 64 * 1024
REDACTED_KEYS = {
    "confirmationcode",
    "localproof",
    "nonce",
    "privatekey",
    "proof",
    "publickey",
    "secret",
    "token",
}


@dataclass(frozen=True)
class DeviceSpec:
    alias: str
    base_url: str


@dataclass(frozen=True)
class EndpointSpec:
    path: str
    interval_seconds: float


def captured_at() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def parse_device_spec(value: str, default_alias: str = "") -> DeviceSpec:
    raw = value.strip()
    if not raw:
        raise ValueError("device must not be empty")
    if "=" in raw and not raw.startswith(("http://", "https://")):
        alias, raw = raw.split("=", 1)
        alias = alias.strip()
    else:
        alias = default_alias
    base_url = raw.strip().rstrip("/")
    parsed = urllib.parse.urlparse(base_url)
    if parsed.scheme not in ("http", "https") or not parsed.hostname:
        raise ValueError(f"invalid device URL: {base_url}")
    if parsed.path not in ("", "/") or parsed.query or parsed.fragment:
        raise ValueError("device URL must not include a path, query, or fragment")
    if not alias:
        alias = parsed.hostname.rsplit(".", 1)[-1]
    if not alias.replace("-", "").replace("_", "").isalnum():
        raise ValueError(f"invalid device alias: {alias}")
    return DeviceSpec(alias=alias[:32], base_url=base_url)


def parse_devices(owner: str, peer: str, extras: list[str]) -> list[DeviceSpec]:
    devices = [
        parse_device_spec(owner, "owner"),
        parse_device_spec(peer, "peer"),
        *(parse_device_spec(value) for value in extras),
    ]
    if len(devices) > MAX_DEVICES:
        raise ValueError(f"at most {MAX_DEVICES} devices are supported")
    aliases = [device.alias for device in devices]
    urls = [device.base_url for device in devices]
    if len(set(aliases)) != len(aliases):
        raise ValueError("device aliases must be unique")
    if len(set(urls)) != len(urls):
        raise ValueError("device URLs must be unique")
    return devices


def redact(value: Any) -> Any:
    if isinstance(value, dict):
        return {
            key: "<redacted>" if key.lower() in REDACTED_KEYS else redact(item)
            for key, item in value.items()
        }
    if isinstance(value, list):
        return [redact(item) for item in value]
    return value


class TimelineWriter:
    def __init__(self, path: Path, started: float) -> None:
        self.path = path
        self.started = started
        self._lock = threading.Lock()
        self._stream = path.open("a", encoding="utf-8", newline="\n")

    def write(self, record: dict[str, Any]) -> None:
        enriched = {
            "schemaVersion": SCHEMA_VERSION,
            "serverCapturedAt": captured_at(),
            "serverElapsedSeconds": round(time.monotonic() - self.started, 3),
            **record,
        }
        with self._lock:
            self._stream.write(json.dumps(enriched, ensure_ascii=False) + "\n")
            self._stream.flush()

    def close(self) -> None:
        with self._lock:
            self._stream.close()


class HttpJsonClient:
    def __init__(self, base_url: str, timeout_seconds: float) -> None:
        self.base_url = base_url
        self.timeout_seconds = timeout_seconds

    def get(self, path: str) -> dict[str, Any]:
        request = urllib.request.Request(
            f"{self.base_url}{path}",
            headers={"Accept": "application/json", "Cache-Control": "no-cache"},
            method="GET",
        )
        started = time.monotonic()
        try:
            with urllib.request.urlopen(request, timeout=self.timeout_seconds) as response:
                status = response.status
                body = response.read()
        except urllib.error.HTTPError as error:
            status = error.code
            body = error.read()
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            return {
                "ok": False,
                "status": None,
                "latencyMs": round((time.monotonic() - started) * 1000, 3),
                "error": str(error)[:300],
            }
        latency_ms = round((time.monotonic() - started) * 1000, 3)
        try:
            payload: Any = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            payload = {"raw": body.decode("utf-8", errors="replace")[:500]}
        return {
            "ok": 200 <= status < 300,
            "status": status,
            "latencyMs": latency_ms,
            "data": redact(payload),
        }


class SnapshotTracker:
    def __init__(self, heartbeat_seconds: float) -> None:
        self.heartbeat_seconds = heartbeat_seconds
        self._last_signature: dict[tuple[str, str], str] = {}
        self._last_written: dict[tuple[str, str], float] = {}

    def should_write(
        self, alias: str, path: str, result: dict[str, Any], now: float
    ) -> tuple[bool, str]:
        comparable = {key: value for key, value in result.items() if key != "latencyMs"}
        signature = json.dumps(comparable, sort_keys=True, separators=(",", ":"))
        key = (alias, path)
        changed = self._last_signature.get(key) != signature
        heartbeat = now - self._last_written.get(key, float("-inf")) >= self.heartbeat_seconds
        if changed or heartbeat or not result.get("ok"):
            self._last_signature[key] = signature
            self._last_written[key] = now
            return True, "change" if changed else "heartbeat"
        return False, "unchanged"


class CaptureState:
    def __init__(
        self,
        writer: TimelineWriter,
        token: str,
        session_id: str,
        owner_url: str,
    ) -> None:
        self.writer = writer
        self.token = token
        self.session_id = session_id
        self.owner_url = owner_url
        self.frontend_events: list[dict[str, Any]] = []
        self._lock = threading.Lock()
        self.event_endpoint = ""

    def accept_frontend(self, event: Any) -> bool:
        if not isinstance(event, dict):
            return False
        if event.get("type") != "frontend" or event.get("sessionId") != self.session_id:
            return False
        safe_event = redact(event)
        with self._lock:
            self.frontend_events.append(safe_event)
        self.writer.write({"type": "frontend", "event": safe_event})
        return True

    def launcher_url(self) -> str:
        url = urllib.parse.urljoin(f"{self.owner_url}/", "dashboard")
        query = urllib.parse.urlencode({
            "tab": "pairing",
            "namoTrace": self.event_endpoint,
            "namoTraceToken": self.token,
            "namoTraceSession": self.session_id,
        })
        return f"{url}?{query}"


class TraceRequestHandler(BaseHTTPRequestHandler):
    server_version = "NamoPairingTrace/1"

    @property
    def capture(self) -> CaptureState:
        return getattr(self.server, "capture")

    def _cors(self) -> None:
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Cache-Control", "no-store")

    def do_OPTIONS(self) -> None:
        self.send_response(204)
        self._cors()
        self.end_headers()

    def do_GET(self) -> None:
        path = urllib.parse.urlparse(self.path).path
        if path == "/":
            self.send_response(302)
            self.send_header("Location", self.capture.launcher_url())
            self._cors()
            self.end_headers()
            return
        if path == "/status":
            body = json.dumps({"ok": True, "sessionId": self.capture.session_id}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self._cors()
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_error(404)

    def do_POST(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        token = urllib.parse.parse_qs(parsed.query).get("token", [""])[0]
        if parsed.path != "/events" or not secrets.compare_digest(token, self.capture.token):
            self.send_error(403)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = 0
        if length < 1 or length > MAX_EVENT_BYTES:
            self.send_error(413)
            return
        try:
            event = json.loads(self.rfile.read(length).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            self.send_error(400)
            return
        if not self.capture.accept_frontend(event):
            self.send_error(400)
            return
        self.send_response(204)
        self._cors()
        self.end_headers()

    def log_message(self, _format: str, *_args: Any) -> None:
        return


def detect_public_host(owner_url: str) -> str:
    parsed = urllib.parse.urlparse(owner_url)
    target = parsed.hostname or "8.8.8.8"
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect((target, parsed.port or 80))
        return sock.getsockname()[0]
    finally:
        sock.close()


def endpoint_specs(poll_seconds: float, slow_poll_seconds: float) -> list[EndpointSpec]:
    return [
        EndpointSpec("/api/platform/pairing", poll_seconds),
        EndpointSpec("/api/platform/participants", poll_seconds),
        EndpointSpec("/api/platform/site-configuration", poll_seconds),
        EndpointSpec("/api/platform/groups", slow_poll_seconds),
        EndpointSpec("/api/floorplan/status", slow_poll_seconds),
        EndpointSpec("/api/system/status", max(10.0, slow_poll_seconds)),
    ]


def collect_one(
    device: DeviceSpec, client: HttpJsonClient, endpoint: EndpointSpec
) -> tuple[DeviceSpec, EndpointSpec, dict[str, Any]]:
    return device, endpoint, client.get(endpoint.path)


def run_polling(
    devices: list[DeviceSpec],
    endpoints: list[EndpointSpec],
    timeout_seconds: float,
    duration_seconds: float,
    writer: TimelineWriter,
    stop: threading.Event,
) -> dict[str, Any]:
    clients = {
        device.alias: HttpJsonClient(device.base_url, timeout_seconds)
        for device in devices
    }
    tracker = SnapshotTracker(heartbeat_seconds=10.0)
    started = time.monotonic()
    next_due = {endpoint.path: started for endpoint in endpoints}
    request_count = 0
    request_errors = 0
    last_results: dict[str, dict[str, Any]] = {}
    endpoint_stats: dict[str, dict[str, Any]] = {}
    interrupted = False
    workers = min(16, max(2, len(devices) * 3))
    try:
        with ThreadPoolExecutor(max_workers=workers) as executor:
            while not stop.is_set():
                now = time.monotonic()
                if duration_seconds > 0 and now - started >= duration_seconds:
                    break
                due = [endpoint for endpoint in endpoints if now >= next_due[endpoint.path]]
                if not due:
                    stop.wait(min(0.1, max(0.0, min(next_due.values()) - now)))
                    continue
                futures = [
                    executor.submit(collect_one, device, clients[device.alias], endpoint)
                    for endpoint in due
                    for device in devices
                ]
                for future in as_completed(futures):
                    device, endpoint, result = future.result()
                    request_count += 1
                    key = f"{device.alias}:{endpoint.path}"
                    stats = endpoint_stats.setdefault(key, {
                        "requests": 0,
                        "errors": 0,
                        "maxLatencyMs": 0.0,
                    })
                    stats["requests"] += 1
                    stats["maxLatencyMs"] = max(
                        stats["maxLatencyMs"], result.get("latencyMs", 0.0)
                    )
                    if not result.get("ok"):
                        request_errors += 1
                        stats["errors"] += 1
                    last_results[key] = result
                    current = time.monotonic()
                    should_write, reason = tracker.should_write(
                        device.alias, endpoint.path, result, current
                    )
                    if should_write:
                        writer.write({
                            "type": "device_snapshot",
                            "reason": reason,
                            "device": device.alias,
                            "baseUrl": device.base_url,
                            "path": endpoint.path,
                            **result,
                        })
                advanced = time.monotonic()
                for endpoint in due:
                    while next_due[endpoint.path] <= advanced:
                        next_due[endpoint.path] += endpoint.interval_seconds
    except KeyboardInterrupt:
        interrupted = True
        stop.set()
    return {
        "requestCount": request_count,
        "requestErrors": request_errors,
        "lastResults": last_results,
        "endpointStats": endpoint_stats,
        "interrupted": interrupted,
    }


def build_summary(
    capture: CaptureState,
    polling: dict[str, Any],
    elapsed_seconds: float,
    interrupted: bool,
) -> dict[str, Any]:
    phases: list[dict[str, Any]] = []
    removal_checkpoints: list[dict[str, Any]] = []
    for event in capture.frontend_events:
        if event.get("state") == "event" and str(event.get("phase", "")).startswith("removal."):
            removal_checkpoints.append({
                "phase": event.get("phase"),
                "detail": event.get("detail"),
            })
        if event.get("state") not in ("end", "error"):
            continue
        phases.append({
            "phase": event.get("phase"),
            "state": event.get("state"),
            "durationMs": event.get("durationMs"),
            "detail": event.get("detail"),
        })
    return {
        "schemaVersion": SCHEMA_VERSION,
        "type": "summary",
        "capturedAt": captured_at(),
        "elapsedSeconds": round(elapsed_seconds, 3),
        "interrupted": interrupted,
        "frontendEventCount": len(capture.frontend_events),
        "requestCount": polling["requestCount"],
        "requestErrors": polling["requestErrors"],
        "endpointStats": polling.get("endpointStats", {}),
        "phases": phases,
        "removalCheckpoints": removal_checkpoints,
    }


def write_report(path: Path, summary: dict[str, Any]) -> None:
    lines = [
        "NAMO pairing trace report",
        f"Elapsed: {summary['elapsedSeconds']} seconds",
        f"Frontend events: {summary['frontendEventCount']}",
        f"Device requests: {summary['requestCount']}",
        f"Device request errors: {summary['requestErrors']}",
        "",
        "Frontend phases:",
    ]
    phases = summary.get("phases", [])
    if not phases:
        lines.append("  No frontend phase events were received.")
    for phase in phases:
        duration = phase.get("durationMs")
        duration_text = f"{duration:.1f} ms" if isinstance(duration, (int, float)) else "n/a"
        lines.append(f"  {phase.get('state', '?'):5} {duration_text:>12}  {phase.get('phase', '?')}")
    lines.extend(["", "Removal checkpoints:"])
    checkpoints = summary.get("removalCheckpoints", [])
    if not checkpoints:
        lines.append("  No removal checkpoint events were received.")
    for checkpoint in checkpoints:
        detail = json.dumps(
            checkpoint.get("detail") or {}, ensure_ascii=False, sort_keys=True
        )
        lines.append(f"  {checkpoint.get('phase', '?')}: {detail}")
    lines.extend(["", "Device endpoint totals:"])
    for endpoint, stats in sorted(summary.get("endpointStats", {}).items()):
        lines.append(
            f"  {endpoint}: requests={stats['requests']} errors={stats['errors']} "
            f"max={stats['maxLatencyMs']:.1f} ms"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Capture a NAMO pairing, calibration, and removal timeline."
    )
    parser.add_argument("--owner", required=True, help="Owner device base URL")
    parser.add_argument("--peer", required=True, help="Peer device base URL")
    parser.add_argument(
        "--device", action="append", default=[], help="Additional alias=URL device"
    )
    parser.add_argument("--duration-seconds", type=float, default=0.0)
    parser.add_argument("--poll-seconds", type=float, default=1.0)
    parser.add_argument("--slow-poll-seconds", type=float, default=2.0)
    parser.add_argument("--timeout-seconds", type=float, default=3.0)
    parser.add_argument("--listen-host", default="0.0.0.0")
    parser.add_argument("--public-host", default="")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--output-dir", default="")
    args = parser.parse_args()

    if args.poll_seconds < 0.5 or args.slow_poll_seconds < 1.0:
        parser.error("poll intervals are too short")
    if args.duration_seconds < 0 or args.timeout_seconds <= 0:
        parser.error("duration and timeout values are invalid")

    try:
        devices = parse_devices(args.owner, args.peer, args.device)
    except ValueError as error:
        parser.error(str(error))

    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    output_dir = Path(args.output_dir) if args.output_dir else Path("captures") / f"pairing-trace-{timestamp}"
    output_dir.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    writer = TimelineWriter(output_dir / "timeline.jsonl", started)
    token = secrets.token_urlsafe(24)
    session_id = secrets.token_hex(8)
    capture = CaptureState(writer, token, session_id, devices[0].base_url)
    server = ThreadingHTTPServer((args.listen_host, args.port), TraceRequestHandler)
    setattr(server, "capture", capture)
    public_host = args.public_host or detect_public_host(devices[0].base_url)
    public_port = server.server_address[1]
    capture.event_endpoint = f"http://{public_host}:{public_port}/events"
    launcher = f"http://{public_host}:{public_port}/"
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    stop = threading.Event()
    interrupted = False
    writer.write({
        "type": "session",
        "sessionId": session_id,
        "devices": {device.alias: device.base_url for device in devices},
        "launcher": launcher,
        "intervals": {
            "pollSeconds": args.poll_seconds,
            "slowPollSeconds": args.slow_poll_seconds,
            "timeoutSeconds": args.timeout_seconds,
        },
    })

    print(f"pairing_trace_capture: output={output_dir.resolve()}")
    print(f"pairing_trace_capture: open this URL on the phone: {launcher}")
    print("pairing_trace_capture: press Ctrl+C after the workflow completes")
    polling: dict[str, Any] = {
        "requestCount": 0,
        "requestErrors": 0,
        "lastResults": {},
        "endpointStats": {},
        "interrupted": False,
    }
    try:
        polling = run_polling(
            devices,
            endpoint_specs(args.poll_seconds, args.slow_poll_seconds),
            args.timeout_seconds,
            args.duration_seconds,
            writer,
            stop,
        )
        interrupted = polling.get("interrupted", False)
    except KeyboardInterrupt:
        interrupted = True
    finally:
        stop.set()
        server.shutdown()
        server.server_close()
        summary = build_summary(
            capture, polling, time.monotonic() - started, interrupted
        )
        writer.write(summary)
        writer.close()
        (output_dir / "summary.json").write_text(
            json.dumps(summary, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        write_report(output_dir / "report.txt", summary)

    print(json.dumps(summary, ensure_ascii=False, indent=2))
    print("pairing_trace_capture: complete")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

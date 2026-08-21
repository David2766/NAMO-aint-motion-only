const TRACE_CONFIG_KEY = "namo-pairing-trace-config";
const TRACE_EVENTS_KEY = "namo-pairing-trace-events";
const MAX_STORED_EVENTS = 128;
const TRACE_CONFIG_MAX_AGE_MS = 12 * 60 * 60 * 1000;

export type PairingTraceState = "begin" | "end" | "error" | "event";

export interface PairingTraceEvent {
  schemaVersion: 1;
  type: "frontend";
  capturedAt: string;
  elapsedMs: number;
  sessionId: string;
  sequence: number;
  phase: string;
  state: PairingTraceState;
  durationMs?: number;
  detail?: Record<string, unknown>;
}

type TraceConfig = {
  endpoint: string;
  token: string;
  sessionId: string;
  startedAtEpochMs: number;
};

export type PairingTraceEmitter = (
  phase: string,
  state: PairingTraceState,
  detail?: Record<string, unknown>,
  durationMs?: number
) => void;

let cachedConfig: TraceConfig | null | undefined;
let sequence = 0;

function validEndpoint(value: string): string | null {
  try {
    const url = new URL(value);
    return url.protocol === "http:" || url.protocol === "https:"
      ? url.toString()
      : null;
  } catch {
    return null;
  }
}

function readStoredConfig(): TraceConfig | null {
  try {
    const raw = window.sessionStorage.getItem(TRACE_CONFIG_KEY);
    if (!raw) return null;
    const value = JSON.parse(raw) as Partial<TraceConfig>;
    const endpoint = validEndpoint(value.endpoint ?? "");
    if (!endpoint || !value.token || !value.sessionId) return null;
    const startedAtEpochMs = value.startedAtEpochMs;
    if (!Number.isFinite(startedAtEpochMs) ||
        Date.now() - startedAtEpochMs! > TRACE_CONFIG_MAX_AGE_MS) return null;
    return {
      endpoint,
      token: value.token,
      sessionId: value.sessionId,
      startedAtEpochMs: startedAtEpochMs!
    };
  } catch {
    return null;
  }
}

function resolveConfig(): TraceConfig | null {
  if (cachedConfig !== undefined) return cachedConfig;
  if (typeof window === "undefined") {
    cachedConfig = null;
    return cachedConfig;
  }
  const params = new URLSearchParams(window.location.search);
  const endpoint = validEndpoint(params.get("namoTrace") ?? "");
  const token = params.get("namoTraceToken") ?? "";
  const sessionId = params.get("namoTraceSession") ?? "";
  if (endpoint && token && sessionId) {
    cachedConfig = {
      endpoint,
      token,
      sessionId,
      startedAtEpochMs: Date.now()
    };
    try {
      window.sessionStorage.setItem(TRACE_CONFIG_KEY, JSON.stringify(cachedConfig));
    } catch {
      // Diagnostics must never affect the pairing flow.
    }
    return cachedConfig;
  }
  cachedConfig = readStoredConfig();
  return cachedConfig;
}

function storeEventCopy(event: PairingTraceEvent): void {
  try {
    const raw = window.sessionStorage.getItem(TRACE_EVENTS_KEY);
    const stored = raw ? JSON.parse(raw) : [];
    const events = Array.isArray(stored) ? stored : [];
    events.push(event);
    if (events.length > MAX_STORED_EVENTS) {
      events.splice(0, events.length - MAX_STORED_EVENTS);
    }
    window.sessionStorage.setItem(TRACE_EVENTS_KEY, JSON.stringify(events));
  } catch {
    // A full or disabled session store must not affect the product flow.
  }
}

function sendEvent(config: TraceConfig, event: PairingTraceEvent): void {
  const url = new URL(config.endpoint);
  url.searchParams.set("token", config.token);
  const body = JSON.stringify(event);
  try {
    if (typeof navigator !== "undefined" && navigator.sendBeacon?.(url, body)) return;
  } catch {
    // Fall through to the non-blocking fetch path.
  }
  try {
    void fetch(url, {
      method: "POST",
      body,
      keepalive: true,
      mode: "cors",
      cache: "no-store",
      credentials: "omit"
    }).catch(() => undefined);
  } catch {
    // Diagnostics are best-effort and must never change the caller's result.
  }
}

export function emitPairingTrace(
  phase: string,
  state: PairingTraceState,
  detail?: Record<string, unknown>,
  durationMs?: number
): void {
  const config = resolveConfig();
  if (!config) return;
  const event: PairingTraceEvent = {
    schemaVersion: 1,
    type: "frontend",
    capturedAt: new Date().toISOString(),
    elapsedMs: Math.max(0, Date.now() - config.startedAtEpochMs),
    sessionId: config.sessionId,
    sequence: ++sequence,
    phase,
    state
  };
  if (durationMs !== undefined) event.durationMs = Math.max(0, durationMs);
  if (detail && Object.keys(detail).length > 0) event.detail = detail;
  storeEventCopy(event);
  sendEvent(config, event);
}

export function pairingTraceErrorDetail(cause: unknown): Record<string, unknown> {
  if (!cause || typeof cause !== "object") return { name: typeof cause };
  const value = cause as { name?: unknown; code?: unknown; status?: unknown };
  const detail: Record<string, unknown> = {};
  if (typeof value.name === "string") detail.name = value.name;
  if (typeof value.code === "string") detail.code = value.code;
  if (typeof value.status === "number") detail.status = value.status;
  return detail;
}

export async function tracePairingPhase<T>(
  phase: string,
  operation: () => Promise<T>,
  detail?: Record<string, unknown>,
  emitter: PairingTraceEmitter = emitPairingTrace
): Promise<T> {
  const started = performance.now();
  emitter(phase, "begin", detail);
  try {
    const result = await operation();
    emitter(phase, "end", detail, performance.now() - started);
    return result;
  } catch (cause) {
    emitter(
      phase,
      "error",
      { ...detail, ...pairingTraceErrorDetail(cause) },
      performance.now() - started
    );
    throw cause;
  }
}

export function resetPairingTraceForTests(): void {
  cachedConfig = undefined;
  sequence = 0;
}

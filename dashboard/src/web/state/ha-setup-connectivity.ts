import type { WebSystemStatus } from "../types";

export type HaSetupConnectivity =
  | "wifi-not-ready"
  | "api-not-ready"
  | "client-waiting"
  | "client-connected";

export function esphomeWifiReady(status: WebSystemStatus | null | undefined): boolean {
  return status?.wifi?.connected === true;
}

export function haSetupConnectivity(status: WebSystemStatus): HaSetupConnectivity {
  if (!esphomeWifiReady(status)) return "wifi-not-ready";
  if (status.api?.serviceReady !== true) return "api-not-ready";

  const clientConnected = status.api.clientConnected ?? status.api.connected;
  return clientConnected === true ? "client-connected" : "client-waiting";
}

export function haRuntimeReady(status: WebSystemStatus | null): boolean {
  if (!status) return false;
  const state = haSetupConnectivity(status);
  return state === "client-waiting" || state === "client-connected";
}

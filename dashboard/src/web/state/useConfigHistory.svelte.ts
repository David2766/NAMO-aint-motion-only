import { normalizeSoftwareConfig } from "../../core/zones";
import type { WebDeviceConfig } from "../types";

interface ConfigHistoryOptions {
  getConfig: () => WebDeviceConfig | null;
  setConfig: (config: WebDeviceConfig) => void;
  onRestore: (config: WebDeviceConfig) => void;
}

export function createConfigHistory({ getConfig, setConfig, onRestore }: ConfigHistoryOptions) {
  let historyPast = $state<WebDeviceConfig[]>([]);
  let historyFuture = $state<WebDeviceConfig[]>([]);

  function pushHistory(): void {
    const config = getConfig();
    if (!config) return;
    historyPast = [...historyPast, cloneConfig(config)].slice(-30);
    historyFuture = [];
  }

  function undo(): void {
    const config = getConfig();
    if (!config || historyPast.length === 0) return;
    const previous = historyPast[historyPast.length - 1];
    historyPast = historyPast.slice(0, -1);
    historyFuture = [cloneConfig(config), ...historyFuture].slice(0, 30);
    restore(previous);
  }

  function redo(): void {
    const config = getConfig();
    if (!config || historyFuture.length === 0) return;
    const next = historyFuture[0];
    historyFuture = historyFuture.slice(1);
    historyPast = [...historyPast, cloneConfig(config)].slice(-30);
    restore(next);
  }

  function restore(nextConfig: WebDeviceConfig): void {
    const normalized = normalizeSoftwareConfig(cloneConfig(nextConfig));
    setConfig(normalized);
    onRestore(normalized);
  }

  return {
    get canUndo() {
      return historyPast.length > 0;
    },
    get canRedo() {
      return historyFuture.length > 0;
    },
    pushHistory,
    undo,
    redo
  };
}

function cloneConfig(value: WebDeviceConfig): WebDeviceConfig {
  return JSON.parse(JSON.stringify(value)) as WebDeviceConfig;
}

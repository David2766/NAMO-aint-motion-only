import { SAVE_DEBOUNCE_MS } from "../../core/constants";
import { stripPlaceholders } from "../../core/zones";
import type { DeviceApi, SaveState, WebDeviceConfig } from "../types";

interface ConfigSaveOptions {
  api: DeviceApi;
  getConfig: () => WebDeviceConfig | null;
  setStatus: (message: string, tone: "ok" | "warn" | "error") => void;
  errorMessage: (error: unknown) => string;
}

export function createConfigSave({ api, getConfig, setStatus, errorMessage }: ConfigSaveOptions) {
  let saveTimer = 0;
  let saveInFlight = false;
  let saveQueued = false;
  let saveState = $state<SaveState>("idle");

  function scheduleSave(): void {
    if (saveInFlight) {
      saveQueued = true;
      saveState = "queued";
      return;
    }
    window.clearTimeout(saveTimer);
    saveState = "pending";
    saveTimer = window.setTimeout(() => {
      void saveConfigNow();
    }, SAVE_DEBOUNCE_MS);
  }

  async function saveConfigNow(): Promise<void> {
    const config = getConfig();
    if (!config) return;
    if (saveInFlight) {
      saveQueued = true;
      saveState = "queued";
      return;
    }
    saveInFlight = true;
    saveQueued = false;
    saveState = "saving";
    try {
      await api.saveConfig(stripPlaceholders(config));
      saveState = "saved";
      setStatus("설정 저장 완료", "ok");
    } catch (error) {
      saveState = "error";
      setStatus(`설정을 저장하지 못했습니다. ${errorMessage(error)}`, "error");
    } finally {
      saveInFlight = false;
      if (saveQueued) scheduleSave();
    }
  }

  function destroy(): void {
    window.clearTimeout(saveTimer);
  }

  return {
    get saveState() {
      return saveState;
    },
    get saveStatusText() {
      return saveStateLabel(saveState);
    },
    scheduleSave,
    saveConfigNow,
    destroy
  };
}

function saveStateLabel(state: SaveState): string {
  if (state === "pending") return "저장 대기";
  if (state === "saving") return "저장 중";
  if (state === "queued") return "추가 저장 대기";
  if (state === "saved") return "저장 완료";
  if (state === "error") return "저장 실패";
  return "변경 없음";
}

interface RadarPollingOptions {
  loadConfig: () => Promise<void>;
  refreshState: () => Promise<void>;
  intervalMs?: number;
  refreshStats?: () => Promise<void>;
  statsIntervalMs?: number;
  refreshFloorplanAvailability?: () => Promise<void>;
  floorplanIntervalMs?: number;
  refreshPlatformDisplay?: () => Promise<void>;
  platformIntervalMs?: number;
}

export function createRadarPolling({
  loadConfig,
  refreshState,
  refreshStats,
  refreshFloorplanAvailability,
  refreshPlatformDisplay,
  intervalMs = 500,
  statsIntervalMs = 30000,
  floorplanIntervalMs = 5000,
  platformIntervalMs = 5000
}: RadarPollingOptions) {
  let pollTimer = 0;
  let statsTimer = 0;
  let floorplanTimer = 0;
  let platformTimer = 0;

  function start(): void {
    void loadConfig();
    void refreshState();
    void refreshStats?.();
    void refreshPlatformDisplay?.();
    pollTimer = window.setInterval(() => {
      void refreshState();
    }, intervalMs);
    if (refreshStats) {
      statsTimer = window.setInterval(() => {
        void refreshStats();
      }, statsIntervalMs);
    }
    if (refreshFloorplanAvailability) {
      floorplanTimer = window.setInterval(() => {
        void refreshFloorplanAvailability();
      }, floorplanIntervalMs);
    }
    if (refreshPlatformDisplay) {
      platformTimer = window.setInterval(() => {
        void refreshPlatformDisplay();
      }, platformIntervalMs);
    }
  }

  function destroy(): void {
    window.clearInterval(pollTimer);
    window.clearInterval(statsTimer);
    window.clearInterval(floorplanTimer);
    window.clearInterval(platformTimer);
  }

  return {
    start,
    destroy
  };
}

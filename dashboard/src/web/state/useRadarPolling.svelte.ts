interface RadarPollingOptions {
  loadConfig: () => Promise<void>;
  refreshState: () => Promise<void>;
  intervalMs?: number;
  refreshStats?: () => Promise<void>;
  statsIntervalMs?: number;
}

export function createRadarPolling({ loadConfig, refreshState, refreshStats, intervalMs = 500, statsIntervalMs = 30000 }: RadarPollingOptions) {
  let pollTimer = 0;
  let statsTimer = 0;

  function start(): void {
    void loadConfig();
    void refreshState();
    void refreshStats?.();
    pollTimer = window.setInterval(() => {
      void refreshState();
    }, intervalMs);
    if (refreshStats) {
      statsTimer = window.setInterval(() => {
        void refreshStats();
      }, statsIntervalMs);
    }
  }

  function destroy(): void {
    window.clearInterval(pollTimer);
    window.clearInterval(statsTimer);
  }

  return {
    start,
    destroy
  };
}

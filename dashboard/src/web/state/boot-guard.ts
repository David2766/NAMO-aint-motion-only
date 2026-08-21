import type { WebSystemStatus } from "../types";

export function bootGuardRemainingSeconds(
  status: WebSystemStatus | null,
  observedAt: number,
  now: number
): number {
  if (!status?.boot?.initialGuardActive) return 0;
  const reportedRemaining = Number(status.boot.remainingSeconds);
  const fallbackRemaining = Math.max(
    0,
    Number(status.boot.guardSeconds ?? 60) - Number(status.firmware?.uptimeSeconds ?? 0)
  );
  const remainingAtSnapshot = Number.isFinite(reportedRemaining)
    ? Math.max(0, reportedRemaining)
    : fallbackRemaining;
  const elapsed = Math.floor(Math.max(0, now - observedAt) / 1000);
  return Math.max(0, Math.ceil(remainingAtSnapshot) - elapsed);
}

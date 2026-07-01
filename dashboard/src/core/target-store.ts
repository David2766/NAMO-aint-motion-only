import type { HeldRadarTarget, ResolvedRadarTarget } from "./types";

export class TargetStore {
  private readonly lastTargets = new Map<number, HeldRadarTarget>();
  private targetSignature = "";

  syncSource(targets: ResolvedRadarTarget[]): void {
    const signature = targets.map((target) => `${target.x || ""}|${target.y || ""}`).join(";");
    if (signature !== this.targetSignature) {
      this.targetSignature = signature;
      this.lastTargets.clear();
    }
  }

  update(
    targets: ResolvedRadarTarget[],
    holdMs: number,
    readNumber: (entityId: string) => number | null,
    now = Date.now()
  ): void {
    this.syncSource(targets);

    targets.forEach((target, index) => {
      const x = readNumber(target.x);
      const y = readNumber(target.y);
      const valid = x !== null && y !== null && !(x === 0 && y === 0);

      if (valid) {
        this.lastTargets.set(index, {
          name: target.name,
          color: target.color,
          x,
          y,
          lastSeen: now,
          active: true
        });
        return;
      }

      const previous = this.lastTargets.get(index);
      if (!previous) return;

      previous.active = now - previous.lastSeen <= holdMs;
      this.lastTargets.set(index, previous);
    });
  }

  activeTargets(holdMs: number, now = Date.now()): HeldRadarTarget[] {
    return [...this.lastTargets.values()].filter((target) => now - target.lastSeen <= holdMs);
  }

  activeCount(holdMs: number, now = Date.now()): number {
    return this.activeTargets(holdMs, now).length;
  }
}


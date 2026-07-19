import type { WebDeviceState, WebTarget } from "./types";

const HELD_TARGET_FALLBACK = {
  id: "target_1",
  name: "T1",
  color: "#ff6b7a"
};

export function presenceDisplayTargets(state: WebDeviceState | null): WebTarget[] {
  if (!state) return [];

  const targets = state.targets ?? [];
  if (targets.some((target) => target.active)) return targets;

  const assist = state.debug?.staticRadar?.assist;
  const held = assist?.heldTarget;
  if (state.presence !== true || assist?.active !== true || !held) return targets;
  if (!Number.isFinite(held.x) || !Number.isFinite(held.y)) return targets;

  const id = held.id || HELD_TARGET_FALLBACK.id;
  const source = targets.find((target) => target.id === id);
  return [
    ...targets,
    {
      id,
      name: source?.name || HELD_TARGET_FALLBACK.name,
      color: source?.color || HELD_TARGET_FALLBACK.color,
      x: Number(held.x),
      y: Number(held.y),
      active: true,
      displayMode: held.distanceMatched === true ? "static-matched" : "static-uncertain"
    }
  ];
}

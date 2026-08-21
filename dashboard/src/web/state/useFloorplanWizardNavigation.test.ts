import { afterEach, describe, expect, it, vi } from "vitest";
import { createFloorplanWizardNavigation } from "./useFloorplanWizardNavigation.svelte";

afterEach(() => {
  vi.useRealTimers();
  vi.unstubAllGlobals();
});

describe("floorplan wizard navigation", () => {
  it("replaces the current step without exposing an animated blank frame", () => {
    vi.useFakeTimers();
    vi.stubGlobal("window", {
      setTimeout: globalThis.setTimeout,
      clearTimeout: globalThis.clearTimeout
    });
    const navigation = createFloorplanWizardNavigation({ transitionMs: 220 });
    const staleCallback = vi.fn();
    const readyCallback = vi.fn();

    navigation.transition("ocr", "forward", staleCallback);
    expect(navigation.animating).toBe(true);

    navigation.replace("rooms", "forward", readyCallback);
    expect(navigation.step).toBe("rooms");
    expect(navigation.animating).toBe(false);
    expect(readyCallback).toHaveBeenCalledOnce();

    vi.advanceTimersByTime(220);
    expect(staleCallback).not.toHaveBeenCalled();
    expect(navigation.step).toBe("rooms");
  });
});

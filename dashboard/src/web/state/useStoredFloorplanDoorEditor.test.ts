import { describe, expect, it, vi } from "vitest";
import type { FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
import { createStoredFloorplanDoorEditor } from "./useStoredFloorplanDoorEditor.svelte";

function documentFixture(): FloorplanStorageDocument {
  return {
    version: 2,
    source: "manual",
    image: { path: "/floorplan.webp", mime: "image/webp", width: 400, height: 300 },
    scale: {
      widthMm: 4000,
      heightMm: 3000,
      outerBoundsPx: [0, 0, 400, 300],
      mmPerPxX: 10,
      mmPerPxY: 10
    },
    radar: { originPx: [100, 200], rotationDeg: 0, scale: 1 },
    outline: { pointsPx: [[0, 0], [400, 0], [400, 300], [0, 300]] },
    wallSegments: [
      { id: "top", axis: "horizontal", x1: 0, y1: 0, x2: 400, y2: 0, length: 400 },
      { id: "right", axis: "vertical", x1: 400, y1: 0, x2: 400, y2: 300, length: 300 }
    ],
    doors: [],
    rooms: [],
    occlusion: { ignoredEdges: [] }
  };
}

describe("stored floorplan door editor", () => {
  it("places a door on frozen physical walls and commits it as a document/config change", () => {
    let document = documentFixture();
    const edit = {
      pushHistory: vi.fn(),
      beginHistoryAction: vi.fn(),
      finishHistoryAction: vi.fn(),
      markChanged: vi.fn()
    };
    const editor = createStoredFloorplanDoorEditor({
      getDocument: () => document,
      setDocument: (next) => (document = next),
      getEditSession: () => edit
    });

    editor.startPlacement();
    expect(editor.placeDoor({ x: 200, y: 4 })).toBe(true);
    editor.finishPlacement();

    expect(document.doors).toHaveLength(1);
    expect(document.doors?.[0]).toMatchObject({ centerPx: [200, 0], rotationDeg: 0 });
    expect(edit.pushHistory).toHaveBeenCalledOnce();
    expect(edit.markChanged).toHaveBeenCalledWith("", ["document", "config"]);
  });

  it("moves and resizes an existing door along the same physical wall", () => {
    let document = {
      ...documentFixture(),
      doors: [{
        id: "manual_door_1",
        kind: "sliding" as const,
        centerPx: [150, 0] as [number, number],
        widthPx: 100,
        rotationDeg: 0
      }]
    };
    const edit = {
      pushHistory: vi.fn(),
      beginHistoryAction: vi.fn(),
      finishHistoryAction: vi.fn(),
      markChanged: vi.fn()
    };
    const editor = createStoredFloorplanDoorEditor({
      getDocument: () => document,
      setDocument: (next) => (document = next as typeof document),
      getEditSession: () => edit
    });

    expect(editor.moveDoor("manual_door_1", { x: 250, y: 6 })).toBe(true);
    expect(document.doors[0].centerPx).toEqual([250, 0]);
    expect(editor.resizeDoor("manual_door_1", "end", { x: 340, y: 0 })).toBe(true);
    expect(document.doors[0].widthPx).toBeGreaterThan(100);
    editor.finishDoorInteraction("manual_door_1", "resize");

    expect(edit.beginHistoryAction).toHaveBeenCalledWith("door-move:manual_door_1");
    expect(edit.beginHistoryAction).toHaveBeenCalledWith("door-resize:manual_door_1");
    expect(edit.finishHistoryAction).toHaveBeenCalledWith("door-resize:manual_door_1");
  });
});

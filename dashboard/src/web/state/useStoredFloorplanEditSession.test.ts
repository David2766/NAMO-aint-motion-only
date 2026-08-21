import { describe, expect, it } from "vitest";
import { createStoredFloorplanEditSession } from "./useStoredFloorplanEditSession.svelte";

function createSession() {
  return createStoredFloorplanEditSession({
    getDocument: () => null,
    getConfig: () => null,
    restoreDocument: () => {},
    getMode: () => "edit",
    getEditTool: () => "rooms",
    setEditTool: () => {},
    getShowRadarOverlay: () => true,
    setShowRadarOverlay: () => {}
  });
}

describe("stored floorplan edit session dirty scopes", () => {
  it("tracks document, config, and room-context scopes independently", () => {
    const session = createSession();

    session.markChanged("", ["config"]);

    expect(session.dirty).toBe(true);
    expect(session.isDirty("config")).toBe(true);
    expect(session.isDirty("document")).toBe(false);
    expect(session.isDirty("roomContext")).toBe(false);
  });

  it("clears all dirty scopes after a successful save", () => {
    const session = createSession();

    session.markChanged("", ["document", "roomContext"]);
    session.markRoomNameChanged("room_1", "Living");
    session.clearDirty();

    expect(session.dirty).toBe(false);
    expect(session.dirtyScopes).toEqual([]);
    expect(session.roomNamePatches).toEqual({});
  });

  it("tracks room-name patches separately from full document changes", () => {
    const session = createSession();

    session.markRoomNameChanged("room_1", "Living");

    expect(session.isDirty("roomName")).toBe(true);
    expect(session.isDirty("roomContext")).toBe(true);
    expect(session.isDirty("document")).toBe(false);
    expect(session.roomNamePatches).toEqual({ room_1: "Living" });
  });

  it("keeps room edits separate from coordinate-frame changes", () => {
    const session = createSession();

    session.markChanged("", ["document", "roomContext"]);

    expect(session.isDirty("document")).toBe(true);
    expect(session.isDirty("coordinateFrame")).toBe(false);
  });

  it("tracks scale edits as coordinate-frame changes", () => {
    const session = createSession();

    session.markChanged("", ["document", "coordinateFrame", "roomContext"]);

    expect(session.isDirty("document")).toBe(true);
    expect(session.isDirty("coordinateFrame")).toBe(true);
  });
});

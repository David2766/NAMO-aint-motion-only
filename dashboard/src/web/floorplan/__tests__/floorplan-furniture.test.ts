import { describe, expect, it } from "vitest";
import {
  clampFurnitureObjectToBounds,
  constrainFurnitureObjectToRooms,
  createDefaultFurnitureObject,
  furnitureAxisAlignedBounds,
  furnitureLocalPointToWorld,
  furnitureRotatedCorners,
  resizeFurnitureObjectFromCorner,
  type FloorplanFurnitureObject,
  type FloorplanFurnitureRoom
} from "../floorplan-furniture";

const leftRoom: FloorplanFurnitureRoom = {
  id: "left",
  pointsPx: [[0, 0], [100, 0], [100, 100], [0, 100]]
};

const rightRoom: FloorplanFurnitureRoom = {
  id: "right",
  pointsPx: [[100, 0], [200, 0], [200, 100], [100, 100]]
};

function furniture(overrides: Partial<FloorplanFurnitureObject> = {}): FloorplanFurnitureObject {
  return {
    id: "object_1",
    asset: "desk",
    roomId: "left",
    xPx: 20,
    yPx: 20,
    widthPx: 30,
    heightPx: 30,
    rotationDeg: 0,
    ...overrides
  };
}

describe("floorplan furniture room constraints", () => {
  it("keeps the selected room id when placing a new object", () => {
    const object = createDefaultFurnitureObject(
      { id: "desk", label: "Desk", url: "/desk.svg", widthRatio: 0.3, heightRatio: 0.2 },
      { x: 0, y: 0, width: 100, height: 100 },
      "object_1",
      "left"
    );

    expect(object.roomId).toBe("left");
    expect(object.xPx).toBeGreaterThanOrEqual(0);
    expect(object.yPx).toBeGreaterThanOrEqual(0);
  });

  it("uses the asset viewBox ratio for the initial furniture box", () => {
    const object = createDefaultFurnitureObject(
      { id: "sofa", label: "Sofa", url: "/sofa.svg", widthRatio: 0.5, heightRatio: 0.28 },
      { x: 0, y: 0, width: 200, height: 200 },
      "object_1",
      "left"
    );

    expect(object.widthPx).toBe(100);
    expect(object.heightPx).toBe(47);
  });

  it("uses rotated corners for furniture bounds", () => {
    const object = furniture({ xPx: 10, yPx: 20, widthPx: 30, heightPx: 10, rotationDeg: 90 });

    expect(furnitureRotatedCorners(object)).toHaveLength(4);
    expect(furnitureAxisAlignedBounds(object)).toEqual({
      x: 20,
      y: 10,
      width: 10,
      height: 30
    });
  });

  it("clamps fallback bounds using the rotated visual box", () => {
    const object = furniture({ xPx: 80, yPx: 20, widthPx: 60, heightPx: 20, rotationDeg: 90 });

    expect(clampFurnitureObjectToBounds(object, { x: 0, y: 0, width: 100, height: 100 })).toMatchObject({
      xPx: 60,
      yPx: 20
    });
  });

  it("keeps the previous rotated furniture when it would cross the room boundary", () => {
    const previous = furniture({ xPx: 20, yPx: 35, widthPx: 60, heightPx: 20, rotationDeg: 90 });
    const proposed = furniture({ xPx: 70, yPx: 35, widthPx: 60, heightPx: 20, rotationDeg: 90 });

    expect(constrainFurnitureObjectToRooms(
      proposed,
      previous,
      [leftRoom],
      { x: 0, y: 0, width: 200, height: 100 }
    )).toEqual(previous);
  });

  it("resizes rotated furniture in local object coordinates", () => {
    const object = furniture({ xPx: 20, yPx: 20, widthPx: 40, heightPx: 20, rotationDeg: 90 });
    const handlePoint = furnitureLocalPointToWorld(object, { x: 40, y: 20 });

    expect(resizeFurnitureObjectFromCorner(object, "se", handlePoint)).toMatchObject({
      xPx: 5,
      yPx: 25,
      widthPx: 60,
      heightPx: 30
    });
  });

  it("keeps the previous object when a move leaves the current room", () => {
    const previous = furniture({ xPx: 20, yPx: 20 });
    const proposed = furniture({ xPx: 80, yPx: 80 });

    expect(constrainFurnitureObjectToRooms(
      proposed,
      previous,
      [leftRoom, rightRoom],
      { x: 0, y: 0, width: 200, height: 100 }
    )).toEqual(previous);
  });

  it("does not switch room ownership while dragging an object that already belongs to a room", () => {
    const previous = furniture({ xPx: 70, yPx: 20 });
    const proposed = furniture({ xPx: 105, yPx: 20 });

    const constrained = constrainFurnitureObjectToRooms(
      proposed,
      previous,
      [leftRoom, rightRoom],
      { x: 0, y: 0, width: 200, height: 100 }
    );

    expect(constrained).toEqual(previous);
  });

  it("assigns a room to legacy furniture without a room id when fully inside one room", () => {
    const proposed = furniture({ roomId: undefined, xPx: 120, yPx: 20 });

    const constrained = constrainFurnitureObjectToRooms(
      proposed,
      null,
      [leftRoom, rightRoom],
      { x: 0, y: 0, width: 200, height: 100 }
    );

    expect(constrained.roomId).toBe("right");
    expect(constrained.xPx).toBe(120);
  });
});

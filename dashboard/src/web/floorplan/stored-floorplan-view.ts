import type { FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
import { rectFromPoints } from "./floorplan-room-edit-geometry";
import { storedFloorplanScaleEstimate } from "./floorplan-scale";

export interface StoredFloorplanViewBounds {
  x: number;
  y: number;
  width: number;
  height: number;
}

export function storedFloorplanViewBounds(
  document: FloorplanStorageDocument | null | undefined
): StoredFloorplanViewBounds {
  const imageWidth = Math.max(1, document?.image?.width ?? 1);
  const imageHeight = Math.max(1, document?.image?.height ?? 1);
  const outline = document?.outline?.pointsPx;
  const contentBounds = outline && outline.length >= 3
    ? rectFromPoints(outline)
    : storedFloorplanScaleEstimate(document?.scale)?.outerBounds;
  if (!contentBounds || contentBounds.width <= 0 || contentBounds.height <= 0) {
    return { x: 0, y: 0, width: imageWidth, height: imageHeight };
  }

  const paddingX = contentBounds.width * 0.1;
  const paddingY = contentBounds.height * 0.1;
  const x = Math.max(0, contentBounds.x - paddingX);
  const y = Math.max(0, contentBounds.y - paddingY);
  const right = Math.min(imageWidth, contentBounds.x + contentBounds.width + paddingX);
  const bottom = Math.min(imageHeight, contentBounds.y + contentBounds.height + paddingY);
  return {
    x,
    y,
    width: Math.max(1, right - x),
    height: Math.max(1, bottom - y)
  };
}

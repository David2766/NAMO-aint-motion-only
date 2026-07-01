import bedUrl from "../img/bed.svg";
import deskUrl from "../img/desk.svg";
import diningTableUrl from "../img/dining_table.svg";
import sofaUrl from "../img/sofa.svg";
import tvConsoleUrl from "../img/tv_console.svg";

export interface FloorplanFurnitureAsset {
  id: string;
  label: string;
  url: string;
  widthRatio: number;
  heightRatio: number;
}

export const FLOORPLAN_FURNITURE_ASSETS: FloorplanFurnitureAsset[] = [
  { id: "bed", label: "침대", url: bedUrl, widthRatio: 0.54, heightRatio: 0.42 },
  { id: "desk", label: "책상", url: deskUrl, widthRatio: 0.34, heightRatio: 0.24 },
  { id: "dining_table", label: "식탁", url: diningTableUrl, widthRatio: 0.44, heightRatio: 0.34 },
  { id: "sofa", label: "소파", url: sofaUrl, widthRatio: 0.5, heightRatio: 0.28 },
  { id: "tv_console", label: "TV장", url: tvConsoleUrl, widthRatio: 0.46, heightRatio: 0.15 }
];

export function findFloorplanFurnitureAsset(id: string): FloorplanFurnitureAsset | null {
  return FLOORPLAN_FURNITURE_ASSETS.find((asset) => asset.id === id) ?? null;
}

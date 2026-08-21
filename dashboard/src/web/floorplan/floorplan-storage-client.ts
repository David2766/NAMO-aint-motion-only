import {
  FLOORPLAN_CONFIG_PATH,
  FLOORPLAN_IMAGE_PATH,
  floorplanStorageJson,
  type FloorplanStorageDocument,
  type FloorplanStorageOcclusion,
  type FloorplanStorageObject,
  type FloorplanStorageRadar
} from "../../core/floorplan/floorplan-storage";
import { parseApiErrorResponse, type ApiInfo } from "../api/api-result";
import { uploadChunkedFormPayload } from "../api/chunked-form-upload";

export const FLOORPLAN_CONFIG_API_PATH = "/api/floorplan";
export const FLOORPLAN_IMAGE_API_PATH = "/api/floorplan/image";
export const FLOORPLAN_STATUS_API_PATH = "/api/floorplan/status";
const FLOORPLAN_DELETE_API_PATH = "/api/floorplan/delete";
export const FLOORPLAN_RADAR_PATCH_API_PATH = "/api/floorplan/radar";
export const FLOORPLAN_ROOM_NAME_PATCH_API_PATH = "/api/floorplan/room-name";
export const FLOORPLAN_OCCLUSION_PATCH_API_PATH = "/api/floorplan/occlusion";
export const FLOORPLAN_OBJECTS_PATCH_API_PATH = "/api/floorplan/objects";
const FLOORPLAN_UPLOAD_START_API_PATH = "/api/floorplan/upload/start";
const FLOORPLAN_UPLOAD_CHUNK_API_PATH = "/api/floorplan/upload/chunk";
const FLOORPLAN_UPLOAD_COMMIT_API_PATH = "/api/floorplan/upload/commit";

export interface FloorplanStorageClientOptions {
  baseUrl?: string;
  fetcher?: typeof fetch;
}

export interface SaveFloorplanStorageInput {
  document: FloorplanStorageDocument;
  image: Blob;
}

export interface FloorplanStorageStatus {
  ok: boolean;
  storage: string;
  partition: string;
  hasConfig: boolean;
  hasImage: boolean;
  totalBytes: number;
  usedBytes: number;
  configBytes: number;
  imageBytes: number;
  configHash?: number;
  imageHash?: number;
  uploadTarget: number;
  uploadSize: number;
  uploadWritten: number;
}

export function hasCompleteFloorplanStorage(status: FloorplanStorageStatus): boolean {
  return status.ok === true && status.hasConfig === true && status.hasImage === true;
}

export function floorplanStorageRevision(status: FloorplanStorageStatus): string {
  return [status.configHash ?? 0, status.imageHash ?? 0, status.configBytes, status.imageBytes].join(":");
}

export type FloorplanStorageErrorInfo = ApiInfo;

export class FloorplanStorageRequestError extends Error {
  constructor(
    message: string,
    public readonly status: number,
    public readonly statusText: string,
    public readonly errorCode?: string,
    public readonly errorInfo?: FloorplanStorageErrorInfo,
    public readonly legacyError?: string
  ) {
    super(message);
    this.name = "FloorplanStorageRequestError";
  }
}

export async function saveFloorplanStorage(
  { document, image }: SaveFloorplanStorageInput,
  options: FloorplanStorageClientOptions = {}
): Promise<void> {
  await uploadPayload("image", await blobToBytes(image), options);
  await uploadPayload("config", textToBytes(floorplanStorageJson(document)), options);
}

export async function saveFloorplanStorageDocument(
  document: FloorplanStorageDocument,
  options: FloorplanStorageClientOptions = {}
): Promise<void> {
  await uploadPayload("config", textToBytes(floorplanStorageJson(document)), options);
}

export async function saveFloorplanRadarPatch(
  radar: FloorplanStorageRadar,
  options: FloorplanStorageClientOptions = {}
): Promise<void> {
  await postForm(FLOORPLAN_RADAR_PATCH_API_PATH, options, { data: JSON.stringify(radar) });
}

export async function saveFloorplanRoomNamePatch(
  roomId: string,
  name: string,
  options: FloorplanStorageClientOptions = {}
): Promise<void> {
  await postForm(FLOORPLAN_ROOM_NAME_PATCH_API_PATH, options, { id: roomId, name });
}

export async function saveFloorplanOcclusionPatch(
  occlusion: FloorplanStorageOcclusion,
  options: FloorplanStorageClientOptions = {}
): Promise<void> {
  await postForm(FLOORPLAN_OCCLUSION_PATCH_API_PATH, options, { data: JSON.stringify(occlusion) });
}

export async function saveFloorplanObjectsPatch(
  objects: FloorplanStorageObject[],
  options: FloorplanStorageClientOptions = {}
): Promise<void> {
  await postForm(FLOORPLAN_OBJECTS_PATCH_API_PATH, options, { data: JSON.stringify(objects) });
}

export async function loadFloorplanStorageDocument(
  options: FloorplanStorageClientOptions = {}
): Promise<FloorplanStorageDocument> {
  const response = await request(FLOORPLAN_CONFIG_API_PATH, options);
  return response.json() as Promise<FloorplanStorageDocument>;
}

export async function loadFloorplanStorageImage(options: FloorplanStorageClientOptions = {}): Promise<Blob> {
  const response = await request(FLOORPLAN_IMAGE_API_PATH, options);
  return response.blob();
}

export async function loadFloorplanStorageStatus(
  options: FloorplanStorageClientOptions = {}
): Promise<FloorplanStorageStatus> {
  const response = await request(FLOORPLAN_STATUS_API_PATH, options);
  return response.json() as Promise<FloorplanStorageStatus>;
}

export async function deleteFloorplanStorage(options: FloorplanStorageClientOptions = {}): Promise<void> {
  await postForm(FLOORPLAN_DELETE_API_PATH, options, { confirm: "1" });
}

export function floorplanStoragePaths() {
  return {
    configPath: FLOORPLAN_CONFIG_PATH,
    imagePath: FLOORPLAN_IMAGE_PATH,
    configApiPath: FLOORPLAN_CONFIG_API_PATH,
    imageApiPath: FLOORPLAN_IMAGE_API_PATH
  };
}

async function requestOk(path: string, options: FloorplanStorageClientOptions, init?: RequestInit): Promise<void> {
  await request(path, options, init);
}

async function postForm(path: string, options: FloorplanStorageClientOptions, values: Record<string, string>): Promise<void> {
  await requestOk(path, options, {
    method: "POST",
    headers: {
      "Content-Type": "application/x-www-form-urlencoded"
    },
    body: new URLSearchParams(values)
  });
}

async function uploadPayload(
  target: "config" | "image",
  bytes: Uint8Array,
  options: FloorplanStorageClientOptions
): Promise<void> {
  await uploadChunkedFormPayload({
    paths: {
      start: FLOORPLAN_UPLOAD_START_API_PATH,
      chunk: FLOORPLAN_UPLOAD_CHUNK_API_PATH,
      commit: FLOORPLAN_UPLOAD_COMMIT_API_PATH
    },
    bytes,
    fields: { target },
    postForm: (path, values) => postForm(path, options, values)
  });
}

async function blobToBytes(blob: Blob): Promise<Uint8Array> {
  return new Uint8Array(await blob.arrayBuffer());
}

function textToBytes(value: string): Uint8Array {
  return new TextEncoder().encode(value);
}

async function request(path: string, options: FloorplanStorageClientOptions, init?: RequestInit): Promise<Response> {
  const fetcher = options.fetcher ?? fetch;
  const response = await fetcher(resolveApiUrl(options.baseUrl ?? "", path), init);
  if (!response.ok) {
    throw await requestError(response);
  }
  return response;
}

async function requestError(response: Response): Promise<FloorplanStorageRequestError> {
  const parsed = await parseApiErrorResponse(response);
  return new FloorplanStorageRequestError(
    parsed.message,
    parsed.status,
    parsed.statusText,
    parsed.code,
    parsed.errorInfo,
    parsed.legacyError ?? parsed.rawText
  );
}

function resolveApiUrl(baseUrl: string, path: string): string {
  const normalizedBaseUrl = baseUrl.trim().replace(/\/+$/, "");
  if (!normalizedBaseUrl) return path;
  const resolvedBaseUrl = /^https?:\/\//i.test(normalizedBaseUrl) || normalizedBaseUrl.startsWith("/")
    ? normalizedBaseUrl
    : `http://${normalizedBaseUrl}`;
  return `${resolvedBaseUrl}${path.startsWith("/") ? path : `/${path}`}`;
}

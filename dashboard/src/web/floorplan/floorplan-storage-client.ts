import {
  FLOORPLAN_CONFIG_PATH,
  FLOORPLAN_IMAGE_PATH,
  floorplanStorageJson,
  type FloorplanStorageDocument
} from "../../core/floorplan/floorplan-storage";

export const FLOORPLAN_CONFIG_API_PATH = "/api/floorplan";
export const FLOORPLAN_IMAGE_API_PATH = "/api/floorplan/image";
export const FLOORPLAN_STATUS_API_PATH = "/api/floorplan/status";
const FLOORPLAN_DELETE_API_PATH = "/api/floorplan/delete";
const FLOORPLAN_UPLOAD_START_API_PATH = "/api/floorplan/upload/start";
const FLOORPLAN_UPLOAD_CHUNK_API_PATH = "/api/floorplan/upload/chunk";
const FLOORPLAN_UPLOAD_COMMIT_API_PATH = "/api/floorplan/upload/commit";
const UPLOAD_CHUNK_BYTES = 192;

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
  uploadTarget: number;
  uploadSize: number;
  uploadWritten: number;
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
  await postForm(FLOORPLAN_UPLOAD_START_API_PATH, options, {
    target,
    size: String(bytes.byteLength)
  });

  for (let offset = 0; offset < bytes.byteLength; offset += UPLOAD_CHUNK_BYTES) {
    const chunk = bytes.subarray(offset, Math.min(offset + UPLOAD_CHUNK_BYTES, bytes.byteLength));
    await postForm(FLOORPLAN_UPLOAD_CHUNK_API_PATH, options, {
      target,
      offset: String(offset),
      data: bytesToHex(chunk)
    });
  }

  await postForm(FLOORPLAN_UPLOAD_COMMIT_API_PATH, options, { target });
}

async function blobToBytes(blob: Blob): Promise<Uint8Array> {
  return new Uint8Array(await blob.arrayBuffer());
}

function textToBytes(value: string): Uint8Array {
  return new TextEncoder().encode(value);
}

function bytesToHex(bytes: Uint8Array): string {
  let hex = "";
  for (const byte of bytes) {
    hex += byte.toString(16).padStart(2, "0");
  }
  return hex;
}

async function request(path: string, options: FloorplanStorageClientOptions, init?: RequestInit): Promise<Response> {
  const fetcher = options.fetcher ?? fetch;
  const response = await fetcher(resolveApiUrl(options.baseUrl ?? "", path), init);
  if (!response.ok) {
    throw new Error(`${response.status} ${response.statusText}`);
  }
  return response;
}

function resolveApiUrl(baseUrl: string, path: string): string {
  const normalizedBaseUrl = baseUrl.trim().replace(/\/+$/, "");
  if (!normalizedBaseUrl) return path;
  return `${normalizedBaseUrl}${path.startsWith("/") ? path : `/${path}`}`;
}

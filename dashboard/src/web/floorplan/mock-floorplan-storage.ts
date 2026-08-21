import { floorplanStorageJson, type FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
import {
  FLOORPLAN_CONFIG_API_PATH,
  FLOORPLAN_IMAGE_API_PATH,
  FLOORPLAN_OBJECTS_PATCH_API_PATH,
  FLOORPLAN_OCCLUSION_PATCH_API_PATH,
  FLOORPLAN_RADAR_PATCH_API_PATH,
  FLOORPLAN_ROOM_NAME_PATCH_API_PATH,
  FLOORPLAN_STATUS_API_PATH,
} from "./floorplan-storage-client";
import {
  mockPlatformFetch,
  mockPlatformRadarPlacement,
  mockPlatformUsesCanonicalFloorplan,
} from "../api/mock-platform";

const DELETE_API_PATH = "/api/floorplan/delete";
const UPLOAD_START_API_PATH = "/api/floorplan/upload/start";
const UPLOAD_CHUNK_API_PATH = "/api/floorplan/upload/chunk";
const UPLOAD_COMMIT_API_PATH = "/api/floorplan/upload/commit";
const CONFIG_KEY = "presence-sensor-demo-floorplan-config";
const IMAGE_KEY = "presence-sensor-demo-floorplan-image";
const IMAGE_MIME_KEY = "presence-sensor-demo-floorplan-image-mime";
const MOCK_ERROR_KEY = "presence-sensor-demo-floorplan-error";
const MOCK_ERROR_QUERY = "mockFloorplanError";

type UploadTarget = "config" | "image";
type MockFloorplanStorageErrorCode =
  | "delete_failed"
  | "missing_confirm"
  | "payload_too_large"
  | "upload_incomplete"
  | "upload_offset_mismatch"
  | "upload_payload_too_large"
  | "upload_session_mismatch"
  | "upload_storage_failed";

type UploadBuffer = {
  target: UploadTarget;
  size: number;
  hex: string;
};

let uploadBuffer: UploadBuffer | null = null;

export function resetMockFloorplanStorage(): void {
  sessionStorage.removeItem(CONFIG_KEY);
  sessionStorage.removeItem(IMAGE_KEY);
  sessionStorage.removeItem(IMAGE_MIME_KEY);
  uploadBuffer = null;
}

export function hasMockFloorplanStorage(): boolean {
  return Boolean(sessionStorage.getItem(CONFIG_KEY) && sessionStorage.getItem(IMAGE_KEY));
}

export function setMockFloorplanStorageFailure(code: MockFloorplanStorageErrorCode | ""): void {
  if (code) {
    localStorage.setItem(MOCK_ERROR_KEY, code);
  } else {
    localStorage.removeItem(MOCK_ERROR_KEY);
  }
}

export async function saveMockFloorplan(document: FloorplanStorageDocument, image: Blob): Promise<void> {
  const plainDocument = plainClone(document);
  sessionStorage.setItem(CONFIG_KEY, floorplanStorageJson(plainDocument));
  sessionStorage.setItem(IMAGE_KEY, await blobToDataUrl(image));
  sessionStorage.setItem(IMAGE_MIME_KEY, image.type || plainDocument.image.mime || "image/webp");
}

export async function mockFloorplanStorageFetch(input: RequestInfo | URL, init?: RequestInit): Promise<Response> {
  const method = String(init?.method ?? "GET").toUpperCase();
  const path = requestPath(input);
  const platformResponse = await mockPlatformFetch(input, init, {
    floorplan: mockFloorplanSnapshot,
  });
  if (platformResponse) return platformResponse;

  if (method === "GET" && path === FLOORPLAN_STATUS_API_PATH) {
    return jsonResponse(mockPlatformUsesCanonicalFloorplan(input) ? storageStatus() : emptyStorageStatus());
  }
  if (method === "GET" && path === FLOORPLAN_CONFIG_API_PATH) {
    return mockPlatformUsesCanonicalFloorplan(input) ? configResponse(input) : errorResponse(404, "Not Found", "config not found", "not_found");
  }
  if (method === "GET" && path === FLOORPLAN_IMAGE_API_PATH) {
    return mockPlatformUsesCanonicalFloorplan(input) ? imageResponse() : errorResponse(404, "Not Found", "image not found", "not_found");
  }
  if (method === "POST" && path === DELETE_API_PATH) {
    const injected = mockFailureResponse(path);
    if (injected) return injected;
    resetMockFloorplanStorage();
    return textResponse("ok");
  }
  if (method === "POST" && path === UPLOAD_START_API_PATH) return mockFailureResponse(path) ?? uploadStartResponse(init);
  if (method === "POST" && path === UPLOAD_CHUNK_API_PATH) return mockFailureResponse(path) ?? uploadChunkResponse(init);
  if (method === "POST" && path === UPLOAD_COMMIT_API_PATH) return mockFailureResponse(path) ?? uploadCommitResponse(init);
  if (method === "POST" && path === FLOORPLAN_RADAR_PATCH_API_PATH) return patchRadarResponse(init);
  if (method === "POST" && path === FLOORPLAN_ROOM_NAME_PATCH_API_PATH) return patchRoomNameResponse(init);
  if (method === "POST" && path === FLOORPLAN_OCCLUSION_PATCH_API_PATH) return patchOcclusionResponse(init);
  if (method === "POST" && path === FLOORPLAN_OBJECTS_PATCH_API_PATH) return patchObjectsResponse(init);

  return errorResponse(404, "Not Found", "mock floorplan endpoint not found", "not_found");
}

function mockFloorplanSnapshot(device: string) {
  const stored = mockPlatformUsesCanonicalFloorplan(device) ? storedDocument() : null;
  const document = stored
    ? { ...stored, radar: mockPlatformRadarPlacement(device, stored) }
    : null;
  const configText = document ? (sessionStorage.getItem(CONFIG_KEY) ?? "") : "";
  const imageDataUrl = document ? (sessionStorage.getItem(IMAGE_KEY) ?? "") : "";
  return {
    configured: Boolean(document && configText && imageDataUrl),
    document,
    configHash: mockContentHash(configText),
    imageHash: mockContentHash(imageDataUrl),
  };
}

function storageStatus() {
  const configText = sessionStorage.getItem(CONFIG_KEY) ?? "";
  const imageDataUrl = sessionStorage.getItem(IMAGE_KEY) ?? "";
  return {
    ok: true,
    storage: "session-storage",
    partition: "browser",
    hasConfig: configText.length > 0,
    hasImage: imageDataUrl.length > 0,
    totalBytes: 2 * 1024 * 1024,
    usedBytes: configText.length + imageDataUrl.length,
    configBytes: configText.length,
    imageBytes: imageDataUrl.length,
    configHash: mockContentHash(configText),
    imageHash: mockContentHash(imageDataUrl),
    uploadTarget: uploadBuffer?.target === "config" ? 1 : uploadBuffer?.target === "image" ? 2 : 0,
    uploadSize: uploadBuffer?.size ?? 0,
    uploadWritten: Math.floor((uploadBuffer?.hex.length ?? 0) / 2),
  };
}

function emptyStorageStatus() {
  return {
    ok: true,
    storage: "session-storage",
    partition: "browser",
    hasConfig: false,
    hasImage: false,
    totalBytes: 2 * 1024 * 1024,
    usedBytes: 0,
    configBytes: 0,
    imageBytes: 0,
    configHash: 0,
    imageHash: 0,
    uploadTarget: 0,
    uploadSize: 0,
    uploadWritten: 0,
  };
}

export function mockContentHash(value: string): number {
  let hash = 2166136261;
  for (let index = 0; index < value.length; index += 1) {
    hash ^= value.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  return hash >>> 0;
}

function configResponse(input: RequestInfo | URL): Response {
  const stored = storedDocument();
  if (!stored) return errorResponse(404, "Not Found", "config not found", "not_found");
  const document = {
    ...stored,
    radar: mockPlatformRadarPlacement(input, stored),
  };
  return new Response(floorplanStorageJson(document), {
    status: 200,
    headers: {
      "Content-Type": "application/json",
    },
  });
}

async function imageResponse(): Promise<Response> {
  const dataUrl = sessionStorage.getItem(IMAGE_KEY);
  const mime = sessionStorage.getItem(IMAGE_MIME_KEY) || "image/webp";
  if (!dataUrl) return errorResponse(404, "Not Found", "image not found", "not_found");
  const bytes = dataUrlToBytes(dataUrl);
  const body = new ArrayBuffer(bytes.byteLength);
  new Uint8Array(body).set(bytes);
  return new Response(body, {
    status: 200,
    headers: {
      "Content-Type": mime,
    },
  });
}

async function uploadStartResponse(init?: RequestInit): Promise<Response> {
  const form = await requestForm(init);
  const target = form.get("target");
  const size = Number(form.get("size") ?? 0);
  if (target !== "config" && target !== "image") return errorResponse(400, "Bad Request", "invalid target", "invalid_request");
  if (!Number.isFinite(size) || size < 0) return errorResponse(400, "Bad Request", "invalid size", "invalid_request");
  uploadBuffer = { target, size, hex: "" };
  return textResponse("ok");
}

async function uploadChunkResponse(init?: RequestInit): Promise<Response> {
  const form = await requestForm(init);
  const target = form.get("target");
  const data = String(form.get("data") ?? "");
  if (!uploadBuffer || uploadBuffer.target !== target) {
    return errorResponse(409, "Conflict", "upload not started", "upload_session_mismatch");
  }
  if (!/^[0-9a-f]*$/iu.test(data)) return errorResponse(400, "Bad Request", "invalid data", "invalid_request");
  uploadBuffer.hex += data.toLowerCase();
  return textResponse("ok");
}

async function uploadCommitResponse(init?: RequestInit): Promise<Response> {
  const form = await requestForm(init);
  const target = form.get("target");
  if (!uploadBuffer || uploadBuffer.target !== target) {
    return errorResponse(409, "Conflict", "upload not started", "upload_session_mismatch");
  }
  const bytes = hexToBytes(uploadBuffer.hex);
  if (bytes.byteLength !== uploadBuffer.size) {
    return errorResponse(409, "Conflict", "upload size mismatch", "upload_incomplete");
  }

  if (uploadBuffer.target === "config") {
    sessionStorage.setItem(CONFIG_KEY, new TextDecoder().decode(bytes));
  } else {
    const mime = sessionStorage.getItem(IMAGE_MIME_KEY) || "image/webp";
    sessionStorage.setItem(IMAGE_KEY, bytesToDataUrl(bytes, mime));
  }
  uploadBuffer = null;
  return textResponse("ok");
}

async function patchRadarResponse(init?: RequestInit): Promise<Response> {
  const form = await requestForm(init);
  const document = storedDocument();
  if (!document) return errorResponse(404, "Not Found", "config not found", "not_found");
  const radar = JSON.parse(String(form.get("data") ?? "{}"));
  sessionStorage.setItem(
    CONFIG_KEY,
    floorplanStorageJson({
      ...document,
      radar,
    }),
  );
  return textResponse("ok");
}

async function patchRoomNameResponse(init?: RequestInit): Promise<Response> {
  const form = await requestForm(init);
  const document = storedDocument();
  if (!document) return errorResponse(404, "Not Found", "config not found", "not_found");
  const roomId = String(form.get("id") ?? "");
  const name = String(form.get("name") ?? "");
  const rooms = document.rooms.map((room) => (room.id === roomId ? { ...room, name } : room));
  if (!rooms.some((room) => room.id === roomId)) return errorResponse(404, "Not Found", "room not found", "not_found");
  sessionStorage.setItem(
    CONFIG_KEY,
    floorplanStorageJson({
      ...document,
      rooms,
    }),
  );
  return textResponse("ok");
}

async function patchOcclusionResponse(init?: RequestInit): Promise<Response> {
  const form = await requestForm(init);
  const document = storedDocument();
  if (!document) return errorResponse(404, "Not Found", "config not found", "not_found");
  const occlusion = JSON.parse(String(form.get("data") ?? "{}"));
  sessionStorage.setItem(
    CONFIG_KEY,
    floorplanStorageJson({
      ...document,
      occlusion,
    }),
  );
  return textResponse("ok");
}

async function patchObjectsResponse(init?: RequestInit): Promise<Response> {
  const form = await requestForm(init);
  const document = storedDocument();
  if (!document) return errorResponse(404, "Not Found", "config not found", "not_found");
  const objects = JSON.parse(String(form.get("data") ?? "[]"));
  sessionStorage.setItem(
    CONFIG_KEY,
    floorplanStorageJson({
      ...document,
      objects,
    }),
  );
  return textResponse("ok");
}

function storedDocument(): FloorplanStorageDocument | null {
  const configText = sessionStorage.getItem(CONFIG_KEY);
  if (!configText) return null;
  return JSON.parse(configText) as FloorplanStorageDocument;
}

function requestPath(input: RequestInfo | URL): string {
  const raw = input instanceof Request ? input.url : String(input);
  return new URL(raw, window.location.origin).pathname;
}

async function requestForm(init?: RequestInit): Promise<URLSearchParams> {
  if (init?.body instanceof URLSearchParams) return init.body;
  return new URLSearchParams(String(init?.body ?? ""));
}

function jsonResponse(value: unknown): Response {
  return new Response(JSON.stringify(value), {
    status: 200,
    headers: {
      "Content-Type": "application/json",
    },
  });
}

function textResponse(text: string, status = 200, statusText = "OK"): Response {
  return new Response(text, {
    status,
    statusText,
    headers: {
      "Content-Type": "text/plain",
    },
  });
}

function mockFailureResponse(path: string): Response | undefined {
  const code = currentMockFailureCode();
  if (!code) return undefined;
  if (!mockFailureAppliesToPath(code, path)) return undefined;
  const failure = mockFailureSpec(code);
  return errorResponse(failure.status, failure.statusText, failure.legacyError, code, failure.detail);
}

function mockFailureAppliesToPath(code: MockFloorplanStorageErrorCode, path: string): boolean {
  if (code === "delete_failed" || code === "missing_confirm") return path === DELETE_API_PATH;
  if (code === "payload_too_large" || code === "upload_payload_too_large") return path === UPLOAD_START_API_PATH;
  if (code === "upload_session_mismatch" || code === "upload_offset_mismatch") return path === UPLOAD_CHUNK_API_PATH;
  if (code === "upload_incomplete" || code === "upload_storage_failed") return path === UPLOAD_COMMIT_API_PATH;
  return false;
}

function currentMockFailureCode(): MockFloorplanStorageErrorCode | "" {
  const queryCode = new URLSearchParams(window.location.search).get(MOCK_ERROR_QUERY);
  const storedCode = queryCode || localStorage.getItem(MOCK_ERROR_KEY) || "";
  return isMockFloorplanStorageErrorCode(storedCode) ? storedCode : "";
}

function isMockFloorplanStorageErrorCode(value: string): value is MockFloorplanStorageErrorCode {
  return [
    "delete_failed",
    "missing_confirm",
    "payload_too_large",
    "upload_incomplete",
    "upload_offset_mismatch",
    "upload_payload_too_large",
    "upload_session_mismatch",
    "upload_storage_failed",
  ].includes(value);
}

function mockFailureSpec(code: MockFloorplanStorageErrorCode): {
  status: number;
  statusText: string;
  legacyError: string;
  detail: Record<string, unknown>;
} {
  switch (code) {
    case "delete_failed":
      return {
        status: 500,
        statusText: "Internal Server Error",
        legacyError: "mock delete failed",
        detail: { target: "floorplan" },
      };
    case "missing_confirm":
      return {
        status: 400,
        statusText: "Bad Request",
        legacyError: "missing confirm",
        detail: { field: "confirm" },
      };
    case "payload_too_large":
    case "upload_payload_too_large":
      return {
        status: 413,
        statusText: "Payload Too Large",
        legacyError: "mock upload payload too large",
        detail: { maxBytes: 65536, receivedBytes: 131072 },
      };
    case "upload_incomplete":
      return {
        status: 409,
        statusText: "Conflict",
        legacyError: "mock upload incomplete",
        detail: { expected: 1024, received: 512 },
      };
    case "upload_offset_mismatch":
      return {
        status: 409,
        statusText: "Conflict",
        legacyError: "mock upload offset mismatch",
        detail: { expected: 384, received: 192 },
      };
    case "upload_session_mismatch":
      return {
        status: 409,
        statusText: "Conflict",
        legacyError: "mock upload session mismatch",
        detail: { expected: "mock-session-a", received: "mock-session-b" },
      };
    case "upload_storage_failed":
      return {
        status: 500,
        statusText: "Internal Server Error",
        legacyError: "mock upload storage failed",
        detail: { target: "image", where: "commit" },
      };
  }
}

function errorResponse(status: number, statusText: string, legacyError: string, code: string, detail: Record<string, unknown> = {}): Response {
  return new Response(
    JSON.stringify({
      ok: false,
      error: legacyError,
      errorInfo: {
        code,
        severity: status >= 500 ? "error" : "warning",
        detail,
      },
    }),
    {
      status,
      statusText,
      headers: {
        "Content-Type": "application/json",
      },
    },
  );
}

function plainClone<T>(value: T): T {
  return JSON.parse(JSON.stringify(value)) as T;
}

function hexToBytes(hex: string): Uint8Array {
  const bytes = new Uint8Array(Math.floor(hex.length / 2));
  for (let index = 0; index < bytes.length; index += 1) {
    bytes[index] = Number.parseInt(hex.slice(index * 2, index * 2 + 2), 16);
  }
  return bytes;
}

function bytesToDataUrl(bytes: Uint8Array, mime: string): string {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return `data:${mime};base64,${btoa(binary)}`;
}

function dataUrlToBytes(dataUrl: string): Uint8Array {
  const base64 = dataUrl.split(",", 2)[1] ?? "";
  const binary = atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index += 1) {
    bytes[index] = binary.charCodeAt(index);
  }
  return bytes;
}

function blobToDataUrl(blob: Blob): Promise<string> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onerror = () => reject(reader.error ?? new Error("Failed to read image"));
    reader.onload = () => resolve(String(reader.result ?? ""));
    reader.readAsDataURL(blob);
  });
}

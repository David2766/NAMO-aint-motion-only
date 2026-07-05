import { floorplanStorageJson, type FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
import {
  FLOORPLAN_CONFIG_API_PATH,
  FLOORPLAN_IMAGE_API_PATH,
  FLOORPLAN_STATUS_API_PATH
} from "./floorplan-storage-client";

const DELETE_API_PATH = "/api/floorplan/delete";
const UPLOAD_START_API_PATH = "/api/floorplan/upload/start";
const UPLOAD_CHUNK_API_PATH = "/api/floorplan/upload/chunk";
const UPLOAD_COMMIT_API_PATH = "/api/floorplan/upload/commit";
const CONFIG_KEY = "presence-sensor-demo-floorplan-config";
const IMAGE_KEY = "presence-sensor-demo-floorplan-image";
const IMAGE_MIME_KEY = "presence-sensor-demo-floorplan-image-mime";

type UploadTarget = "config" | "image";

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

export async function saveMockFloorplan(document: FloorplanStorageDocument, image: Blob): Promise<void> {
  const plainDocument = plainClone(document);
  sessionStorage.setItem(CONFIG_KEY, floorplanStorageJson(plainDocument));
  sessionStorage.setItem(IMAGE_KEY, await blobToDataUrl(image));
  sessionStorage.setItem(IMAGE_MIME_KEY, image.type || plainDocument.image.mime || "image/webp");
}

export async function mockFloorplanStorageFetch(input: RequestInfo | URL, init?: RequestInit): Promise<Response> {
  const method = String(init?.method ?? "GET").toUpperCase();
  const path = requestPath(input);

  if (method === "GET" && path === FLOORPLAN_STATUS_API_PATH) return jsonResponse(storageStatus());
  if (method === "GET" && path === FLOORPLAN_CONFIG_API_PATH) return configResponse();
  if (method === "GET" && path === FLOORPLAN_IMAGE_API_PATH) return imageResponse();
  if (method === "POST" && path === DELETE_API_PATH) {
    resetMockFloorplanStorage();
    return textResponse("ok");
  }
  if (method === "POST" && path === UPLOAD_START_API_PATH) return uploadStartResponse(init);
  if (method === "POST" && path === UPLOAD_CHUNK_API_PATH) return uploadChunkResponse(init);
  if (method === "POST" && path === UPLOAD_COMMIT_API_PATH) return uploadCommitResponse(init);

  return textResponse("mock floorplan endpoint not found", 404, "Not Found");
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
    uploadTarget: uploadBuffer?.target === "config" ? 1 : uploadBuffer?.target === "image" ? 2 : 0,
    uploadSize: uploadBuffer?.size ?? 0,
    uploadWritten: Math.floor((uploadBuffer?.hex.length ?? 0) / 2)
  };
}

function configResponse(): Response {
  const configText = sessionStorage.getItem(CONFIG_KEY);
  if (!configText) return textResponse("config not found", 404, "Not Found");
  return new Response(configText, {
    status: 200,
    headers: {
      "Content-Type": "application/json"
    }
  });
}

async function imageResponse(): Promise<Response> {
  const dataUrl = sessionStorage.getItem(IMAGE_KEY);
  const mime = sessionStorage.getItem(IMAGE_MIME_KEY) || "image/webp";
  if (!dataUrl) return textResponse("image not found", 404, "Not Found");
  const bytes = dataUrlToBytes(dataUrl);
  return new Response(bytes, {
    status: 200,
    headers: {
      "Content-Type": mime
    }
  });
}

async function uploadStartResponse(init?: RequestInit): Promise<Response> {
  const form = await requestForm(init);
  const target = form.get("target");
  const size = Number(form.get("size") ?? 0);
  if (target !== "config" && target !== "image") return textResponse("invalid target", 400, "Bad Request");
  if (!Number.isFinite(size) || size < 0) return textResponse("invalid size", 400, "Bad Request");
  uploadBuffer = { target, size, hex: "" };
  return textResponse("ok");
}

async function uploadChunkResponse(init?: RequestInit): Promise<Response> {
  const form = await requestForm(init);
  const target = form.get("target");
  const data = String(form.get("data") ?? "");
  if (!uploadBuffer || uploadBuffer.target !== target) return textResponse("upload not started", 400, "Bad Request");
  if (!/^[0-9a-f]*$/iu.test(data)) return textResponse("invalid data", 400, "Bad Request");
  uploadBuffer.hex += data.toLowerCase();
  return textResponse("ok");
}

async function uploadCommitResponse(init?: RequestInit): Promise<Response> {
  const form = await requestForm(init);
  const target = form.get("target");
  if (!uploadBuffer || uploadBuffer.target !== target) return textResponse("upload not started", 400, "Bad Request");
  const bytes = hexToBytes(uploadBuffer.hex);
  if (bytes.byteLength !== uploadBuffer.size) return textResponse("upload size mismatch", 400, "Bad Request");

  if (uploadBuffer.target === "config") {
    sessionStorage.setItem(CONFIG_KEY, new TextDecoder().decode(bytes));
  } else {
    const mime = sessionStorage.getItem(IMAGE_MIME_KEY) || "image/webp";
    sessionStorage.setItem(IMAGE_KEY, bytesToDataUrl(bytes, mime));
  }
  uploadBuffer = null;
  return textResponse("ok");
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
      "Content-Type": "application/json"
    }
  });
}

function textResponse(text: string, status = 200, statusText = "OK"): Response {
  return new Response(text, {
    status,
    statusText,
    headers: {
      "Content-Type": "text/plain"
    }
  });
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

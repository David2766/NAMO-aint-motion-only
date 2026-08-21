import { formatBytes } from "../utils/formatters";

const MAX_UPLOAD_BYTES = 5 * 1024 * 1024;
const MAX_WEBP_BYTES = 80 * 1024;
const MIN_LONG_SIDE = 800;
const MIN_SHORT_SIDE = 500;
const QUALITY_STEPS = [0.9, 0.85, 0.8, 0.75, 0.7];
const ALLOWED_TYPES = new Set(["image/png", "image/jpeg", "image/webp"]);

interface FloorplanImageSessionText {
  unsupportedImageType: string;
  imageTooLarge: string;
  imageTooSmall: string;
  webpTooLarge: string;
  canvasUnavailable: string;
  webpFailed: string;
  canUseEsp32Candidate: string;
  selectImageStatus: string;
}

export interface PreparedFloorplanCandidate {
  ok: boolean;
  reason: string;
  blob: Blob;
  originalName: string;
  originalType: string;
  originalSize: number;
  width: number;
  height: number;
  longSide: number;
  shortSide: number;
  outputType: "image/webp";
  outputSize: number;
  quality: number;
  resized: false;
  attempts: Array<{
    quality: number;
    size: number;
    ok: boolean;
  }>;
}

export type FloorplanImageDebugInfo = Partial<PreparedFloorplanCandidate> & {
  width: number;
  height: number;
  size?: number;
  type?: string;
  ok?: boolean;
};

interface FloorplanImageSessionOptions {
  getText: () => FloorplanImageSessionText;
  formatError: (error: unknown) => string;
  onBeforeLoad?: () => void;
  onLoaded?: (result: PreparedFloorplanCandidate) => void;
}

interface GeneratedFloorplanCandidate {
  blob: Blob;
  name: string;
  width: number;
  height: number;
}

export function createFloorplanImageSession({
  getText,
  formatError,
  onBeforeLoad,
  onLoaded
}: FloorplanImageSessionOptions) {
  let imageUrl = $state("");
  let imageName = $state("");
  let imageMeta = $state("");
  let statusTone = $state("warn");
  let statusText = $state("");
  let debugInfo = $state<FloorplanImageDebugInfo | null>(null);
  let busy = $state(false);
  let imageBlob = $state<Blob | null>(null);
  let originalImageBlob = $state<Blob | null>(null);

  async function handleFileChange(event: Event): Promise<void> {
    const input = event.currentTarget as HTMLInputElement | null;
    const file = input?.files?.[0];
    if (input) input.value = "";
    if (!file) return;
    busy = true;
    clear();
    onBeforeLoad?.();

    try {
      const result = await prepareCandidate(file, getText());
      imageUrl = URL.createObjectURL(result.blob);
      imageBlob = result.blob;
      originalImageBlob = file;
      imageName = file.name;
      imageMeta = `${formatBytes(result.blob.size, {
        unitSeparator: " ",
        minUnit: "KB",
        mbPrecision: 2
      })} · ${result.width} x ${result.height}px · WebP`;
      statusTone = result.ok ? "ok" : "error";
      statusText = result.ok ? getText().canUseEsp32Candidate : result.reason;
      debugInfo = result;
      onLoaded?.(result);
    } catch (error) {
      statusTone = "error";
      statusText = formatError(error);
      debugInfo = null;
    } finally {
      busy = false;
    }
  }

  function clear(): void {
    if (imageUrl) URL.revokeObjectURL(imageUrl);
    imageUrl = "";
    imageName = "";
    imageMeta = "";
    debugInfo = null;
    imageBlob = null;
    originalImageBlob = null;
    statusTone = "warn";
    statusText = getText().selectImageStatus;
  }

  function setGeneratedCandidate(candidate: GeneratedFloorplanCandidate): void {
    if (imageUrl) URL.revokeObjectURL(imageUrl);
    imageUrl = URL.createObjectURL(candidate.blob);
    imageBlob = candidate.blob;
    originalImageBlob = null;
    imageName = candidate.name;
    imageMeta = `${formatBytes(candidate.blob.size, {
      unitSeparator: " ",
      minUnit: "KB",
      mbPrecision: 2
    })} · ${candidate.width} x ${candidate.height}px · WebP`;
    statusTone = "ok";
    statusText = getText().canUseEsp32Candidate;
    debugInfo = {
      width: candidate.width,
      height: candidate.height,
      size: candidate.blob.size,
      type: candidate.blob.type,
      ok: true
    };
  }

  function destroy(): void {
    if (imageUrl) URL.revokeObjectURL(imageUrl);
    imageUrl = "";
  }

  function ensureDefaultStatus(): void {
    if (!imageUrl) statusText = getText().selectImageStatus;
    else if (statusTone === "ok") statusText = getText().canUseEsp32Candidate;
  }

  function setDebugInfo(nextDebugInfo: FloorplanImageDebugInfo | null): void {
    debugInfo = nextDebugInfo;
  }

  return {
    get imageUrl() {
      return imageUrl;
    },
    get imageName() {
      return imageName;
    },
    get imageMeta() {
      return imageMeta;
    },
    get statusTone() {
      return statusTone;
    },
    get statusText() {
      return statusText;
    },
    get debugInfo() {
      return debugInfo;
    },
    get busy() {
      return busy;
    },
    get imageBlob() {
      return imageBlob;
    },
    get originalImageBlob() {
      return originalImageBlob;
    },
    handleFileChange,
    setGeneratedCandidate,
    clear,
    destroy,
    ensureDefaultStatus,
    setDebugInfo
  };
}

async function prepareCandidate(file: File, text: FloorplanImageSessionText): Promise<PreparedFloorplanCandidate> {
  if (!ALLOWED_TYPES.has(file.type)) {
    throw new Error(text.unsupportedImageType);
  }
  if (file.size > MAX_UPLOAD_BYTES) {
    throw new Error(text.imageTooLarge);
  }

  const bitmap = await createImageBitmap(file);
  try {
    const longSide = Math.max(bitmap.width, bitmap.height);
    const shortSide = Math.min(bitmap.width, bitmap.height);
    const resolutionOk = longSide >= MIN_LONG_SIDE && shortSide >= MIN_SHORT_SIDE;
    const attempts: PreparedFloorplanCandidate["attempts"] = [];
    let best: { blob: Blob; quality: number } | null = null;

    for (const quality of QUALITY_STEPS) {
      const blob = await encodeWebp(bitmap, quality, text);
      const attempt = {
        quality,
        size: blob.size,
        ok: blob.size <= MAX_WEBP_BYTES
      };
      attempts.push(attempt);
      if (!best || blob.size < best.blob.size) {
        best = { blob, quality };
      }
      if (attempt.ok) {
        best = { blob, quality };
        break;
      }
    }

    if (!best) throw new Error(text.webpFailed);
    const sizeOk = best.blob.size <= MAX_WEBP_BYTES;
    const ok = resolutionOk && sizeOk;
    const reason = !resolutionOk
      ? text.imageTooSmall
      : !sizeOk
        ? text.webpTooLarge
        : "";

    return {
      ok,
      reason,
      blob: best.blob,
      originalName: file.name,
      originalType: file.type,
      originalSize: file.size,
      width: bitmap.width,
      height: bitmap.height,
      longSide,
      shortSide,
      outputType: "image/webp",
      outputSize: best.blob.size,
      quality: best.quality,
      resized: false,
      attempts
    };
  } finally {
    bitmap.close?.();
  }
}

async function encodeWebp(bitmap: ImageBitmap, quality: number, text: FloorplanImageSessionText): Promise<Blob> {
  const canvas = document.createElement("canvas");
  canvas.width = bitmap.width;
  canvas.height = bitmap.height;
  const context = canvas.getContext("2d", { alpha: false });
  if (!context) throw new Error(text.canvasUnavailable);
  context.fillStyle = "#ffffff";
  context.fillRect(0, 0, canvas.width, canvas.height);
  context.drawImage(bitmap, 0, 0);
  const blob = await new Promise<Blob | null>((resolve) => canvas.toBlob(resolve, "image/webp", quality));
  if (!blob) throw new Error(text.webpFailed);
  return blob;
}

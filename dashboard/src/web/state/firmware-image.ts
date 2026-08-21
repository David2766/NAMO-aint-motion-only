export interface FirmwareImageMetadata {
  version: string;
  projectName: string;
  buildTime: string;
  frameworkVersion: string;
}

const ESP_IMAGE_HEADER_SIZE = 24;
const ESP_SEGMENT_HEADER_SIZE = 8;
const ESP_APP_DESC_OFFSET = ESP_IMAGE_HEADER_SIZE + ESP_SEGMENT_HEADER_SIZE;
const ESP_APP_DESC_MAGIC = 0xabcd5432;
const ESP_IMAGE_MAGIC = 0xe9;
const ESP_APP_DESC_READ_SIZE = 160;
const NAMO_VERSION_PREFIX = "Project truststorm.presence-sensor version ";

function readCString(bytes: Uint8Array, offset: number, size: number): string {
  const end = Math.min(bytes.length, offset + size);
  let value = "";
  for (let index = offset; index < end; index += 1) {
    const byte = bytes[index];
    if (byte === 0) break;
    if (byte < 0x20 || byte > 0x7e) throw new Error("firmware_metadata_invalid");
    value += String.fromCharCode(byte);
  }
  return value.trim();
}

export function parseEspFirmwareMetadata(bytes: Uint8Array): FirmwareImageMetadata {
  if (bytes.length < ESP_APP_DESC_OFFSET + ESP_APP_DESC_READ_SIZE || bytes[0] !== ESP_IMAGE_MAGIC) {
    throw new Error("firmware_image_invalid");
  }

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (view.getUint32(ESP_APP_DESC_OFFSET, true) !== ESP_APP_DESC_MAGIC) {
    throw new Error("firmware_image_invalid");
  }

  const frameworkVersion = readCString(bytes, ESP_APP_DESC_OFFSET + 16, 32);
  const projectName = readCString(bytes, ESP_APP_DESC_OFFSET + 48, 32);
  const time = readCString(bytes, ESP_APP_DESC_OFFSET + 80, 16);
  const date = readCString(bytes, ESP_APP_DESC_OFFSET + 96, 16);
  if (!frameworkVersion || projectName !== "presence-sensor") throw new Error("firmware_metadata_invalid");

  const prefix = new TextEncoder().encode(NAMO_VERSION_PREFIX);
  let versionOffset = -1;
  for (let index = 0; index <= bytes.length - prefix.length; index += 1) {
    let matches = true;
    for (let prefixIndex = 0; prefixIndex < prefix.length; prefixIndex += 1) {
      if (bytes[index + prefixIndex] !== prefix[prefixIndex]) {
        matches = false;
        break;
      }
    }
    if (matches) {
      versionOffset = index + prefix.length;
      break;
    }
  }
  if (versionOffset < 0) throw new Error("firmware_metadata_invalid");
  const versionMatch = /^\d+\.\d+\.\d+(?:-[A-Za-z0-9_-]+)?/.exec(readCString(bytes, versionOffset, 32));
  if (!versionMatch) throw new Error("firmware_metadata_invalid");

  return {
    version: versionMatch[0],
    projectName,
    buildTime: [date, time].filter(Boolean).join(" "),
    frameworkVersion
  };
}

export async function readEspFirmwareMetadata(file: Blob): Promise<FirmwareImageMetadata> {
  const bytes = new Uint8Array(await file.arrayBuffer());
  return parseEspFirmwareMetadata(bytes);
}

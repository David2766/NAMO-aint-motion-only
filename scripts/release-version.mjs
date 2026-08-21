const VERSION_PATTERN = /^(\d+)\.(\d+)\.(\d+)(?:-([a-z]))?$/;

export const RELEASE_TYPES = new Set(["patch", "minor", "major"]);

export function parseSemanticVersion(value) {
  const text = String(value ?? "").trim();
  const match = VERSION_PATTERN.exec(text);
  if (!match) {
    throw new Error(`unsupported semantic version: ${text}`);
  }
  return {
    major: Number(match[1]),
    minor: Number(match[2]),
    patch: Number(match[3]),
    suffix: match[4] ?? ""
  };
}

export function normalizeReleaseType(value = "patch") {
  const releaseType = String(value).trim().toLowerCase();
  if (!RELEASE_TYPES.has(releaseType)) {
    throw new Error(`unsupported release type: ${value}`);
  }
  return releaseType;
}

export function nextReleaseVersion(current, releaseType = "patch") {
  const parsed = parseSemanticVersion(current);
  const type = normalizeReleaseType(releaseType);
  if (type === "major") {
    return `${parsed.major + 1}.0.0`;
  }
  if (type === "minor") {
    return `${parsed.major}.${parsed.minor + 1}.0`;
  }
  return `${parsed.major}.${parsed.minor}.${parsed.patch + 1}`;
}

export function nextDashboardBuildVersion(current, {
  releasePrebumped = false
} = {}) {
  const parsed = parseSemanticVersion(current);
  if (releasePrebumped) {
    return String(current).trim();
  }
  if (!parsed.suffix) {
    return `${parsed.major}.${parsed.minor}.${parsed.patch}-a`;
  }
  if (parsed.suffix === "z") {
    return `${parsed.major}.${parsed.minor}.${parsed.patch + 1}-a`;
  }
  return `${parsed.major}.${parsed.minor}.${parsed.patch}-${String.fromCharCode(parsed.suffix.charCodeAt(0) + 1)}`;
}

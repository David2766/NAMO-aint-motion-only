import { copyFileSync, existsSync, readFileSync, renameSync, rmSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { createHash, createPublicKey } from "node:crypto";

import {
  embeddedDashboardVersion,
  syncEmbeddedFirmwareVersion
} from "./dashboard-asset-version.mjs";
import {
  assertFirmwareTrustEpochMatches,
  firmwareBuildRequiresClean,
  firmwareSubstitutions,
  nextFirmwareSecurityEpoch,
  parseFirmwareBuildArgs,
  shouldRestoreFirmwareTrustHeaderAfterSuccess
} from "./firmware-build-options.mjs";
import { writeTextFileSync } from "./reliable-write.mjs";
import {
  createFirmwarePackage,
  assertFirmwareEmbedsPublicKeys,
  assertFirmwareSigningIdentity,
  assertSignedOtaFirmwareImage,
  loadPrivateKey,
  loadPublicKey,
  p256KeyId,
  readEspFirmwareVersion,
  rawP256PublicKey,
  verifyFirmwarePackage
} from "./firmware-signing.mjs";
import { preparePlatformioEnvironment } from "./prepare-platformio-environment.mjs";
import { nextReleaseVersion } from "./release-version.mjs";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const dashboardDir = resolve(repoRoot, "dashboard");
const versionPath = resolve(repoRoot, "version.json");
const firmwareYamlPath = resolve(repoRoot, "namo.yaml");
const dashboardAssetsPath = resolve(repoRoot, "components", "radar_api_server", "dashboard_assets.h");
const firmwareTrustKeysPath = resolve(repoRoot, "components", "radar_api_server", "firmware_trust_keys.h");
const defaultFirmwareEntry = "namo.yaml";
const defaultBuildEnvName = "presence-sensor";
const releaseBuildEnvName = "presence-sensor-build";

function usage() {
  console.log([
    "Usage:",
    "  node scripts/compile-firmware.mjs [options] [firmware.yaml]",
    "",
    "Options:",
    "  --beta          Build normally and write beta-named OTA/factory binaries",
    "  --hotfix        Build an official patch release and advance the security epoch",
    "  --release       Build an official release and advance the security epoch",
    "  --release-type <type>  Select patch, minor, or major (default: patch)",
    "  --signer <role> Sign with none, developer, or release credentials",
    "  --signed-developer  Compatibility alias for --signer developer",
    "  --dashboard     Build and embed the dashboard before compile",
    "  --clean         Run ESPHome clean before compile",
    "  --cmake-clean   Alias for --clean",
    "  --config        Run ESPHome config before compile",
    "  --transport-smoke  Build an insecure local-only shadow transport test image",
    "  --production-fusion  Compatibility alias; production fusion is enabled by default",
    "  --help          Show this help",
    "",
    "Normal build:",
    "  0.5.0 -> 0.5.0-a -> 0.5.0-b -> ... -> 0.5.0-z -> 0.5.0-a",
    "  --beta follows the same version/build rules and also writes exportable binaries",
    "",
    "Release build:",
    "  patch: 0.5.5-o -> 0.5.6; dashboard 0.4.13-k -> 0.4.14",
    "  minor: 0.5.5-o -> 0.6.0; dashboard 0.4.13-k -> 0.5.0",
    "  major: 0.5.5-o -> 1.0.0; dashboard 0.4.13-k -> 1.0.0",
    "",
    "Signed release build:",
    "  --release --signer release",
    "  --hotfix --signer release",
    "",
    "Release version work also forces:",
    "  --dashboard, --clean, --config, and release-named OTA/factory binaries",
    "  README firmware version sync after a successful release build"
  ].join("\n"));
}

function fail(message) {
  console.error(`compile-firmware: ${message}`);
  process.exit(1);
}

function formatLocalDateTime(date) {
  const pad = (value) => String(value).padStart(2, "0");
  const year = date.getFullYear();
  const month = pad(date.getMonth() + 1);
  const day = pad(date.getDate());
  const hour = pad(date.getHours());
  const minute = pad(date.getMinutes());
  const second = pad(date.getSeconds());
  const offsetMinutes = -date.getTimezoneOffset();
  const offsetSign = offsetMinutes >= 0 ? "+" : "-";
  const offsetAbs = Math.abs(offsetMinutes);
  const offsetHour = pad(Math.floor(offsetAbs / 60));
  const offsetMinute = pad(offsetAbs % 60);
  return `${year}-${month}-${day} ${hour}:${minute}:${second} ${offsetSign}${offsetHour}:${offsetMinute}`;
}

function formatDuration(ms) {
  const totalSeconds = Math.max(0, Math.round(ms / 1000));
  const minutes = Math.floor(totalSeconds / 60);
  const seconds = totalSeconds % 60;
  return `${minutes}m ${seconds}s`;
}

function parseArgs() {
  try {
    return parseFirmwareBuildArgs(process.argv.slice(2), {
      defaultFirmwareEntry
    });
  } catch (error) {
    fail(error instanceof Error ? error.message : String(error));
  }
}

function parseFirmwareVersion(value) {
  const match = /^(\d+)\.(\d+)\.(\d+)(?:-([a-z]))?$/.exec(value);
  if (!match) {
    fail(`unsupported firmware version: ${value}`);
  }
  return {
    major: Number(match[1]),
    minor: Number(match[2]),
    patch: Number(match[3]),
    suffix: match[4] ?? ""
  };
}

function nextFirmwareVersion(current, release, releaseType = "patch") {
  const parsed = parseFirmwareVersion(current);
  if (release) {
    return nextReleaseVersion(current, releaseType);
  }
  if (!parsed.suffix) {
    return `${parsed.major}.${parsed.minor}.${parsed.patch}-a`;
  }
  if (parsed.suffix === "z") {
    return `${parsed.major}.${parsed.minor}.${parsed.patch}-a`;
  }
  return `${parsed.major}.${parsed.minor}.${parsed.patch}-${String.fromCharCode(parsed.suffix.charCodeAt(0) + 1)}`;
}

function readTextIfExists(path) {
  return existsSync(path) ? readFileSync(path, "utf8") : null;
}

function restore(snapshot) {
  writeTextFileSync(versionPath, snapshot.versionText);
  writeTextFileSync(firmwareYamlPath, snapshot.firmwareYamlText);
  if (snapshot.dashboardAssetsText != null) {
    writeTextFileSync(dashboardAssetsPath, snapshot.dashboardAssetsText);
  }
  writeTextFileSync(firmwareTrustKeysPath, snapshot.firmwareTrustKeysText);
}

function requiredSigningEnv(name) {
  const value = String(process.env[name] ?? "").trim();
  if (!value) fail(`${name} is required for a signed build`);
  return value;
}

function cppBytes(value) {
  return `{${[...value].map((byte) => `0x${byte.toString(16).padStart(2, "0")}`).join(", ")}}`;
}

function releaseSigningContext(version, buildId) {
  const rootPublicPath = resolve(requiredSigningEnv("NAMO_ROOT_PUBLIC_KEY"));
  const releasePrivatePath = resolve(requiredSigningEnv("NAMO_RELEASE_PRIVATE_KEY"));
  const developerPublicPath = resolve(requiredSigningEnv("NAMO_DEVELOPER_PUBLIC_KEY"));
  const passphrase = requiredSigningEnv("NAMO_RELEASE_KEY_PASSPHRASE");
  const trustRevision = Number(requiredSigningEnv("NAMO_TRUST_REVISION"));
  const securityEpoch = Number(version.firmwareSecurityEpoch);
  if (!Number.isSafeInteger(trustRevision) || trustRevision < 1 || trustRevision > 0xffffffff)
    fail("NAMO_TRUST_REVISION must be a positive uint32");
  if (!Number.isSafeInteger(securityEpoch) || securityEpoch < 1 || securityEpoch > 0xffffffff)
    fail("version.json firmwareSecurityEpoch must be a positive uint32");
  for (const path of [rootPublicPath, releasePrivatePath, developerPublicPath]) {
    if (!existsSync(path)) fail(`signing key file not found: ${path}`);
  }
  const rootPublic = loadPublicKey(readFileSync(rootPublicPath));
  const releasePrivate = loadPrivateKey(readFileSync(releasePrivatePath), passphrase);
  const releasePublic = createPublicKey(releasePrivate);
  const developerPublic = loadPublicKey(readFileSync(developerPublicPath));
  assertFirmwareSigningIdentity({
    role: "release",
    privateKey: releasePrivate,
    rootPublicKey: rootPublic,
    releasePublicKey: releasePublic,
    developerPublicKey: developerPublic
  });
  return {
    rootPublic,
    releasePrivate,
    releasePublic,
    developerPublic,
    trustRevision,
    securityEpoch,
    buildId
  };
}

function developerSigningContext(version, buildId) {
  const rootPublicPath = resolve(requiredSigningEnv("NAMO_ROOT_PUBLIC_KEY"));
  const releasePublicPath = resolve(requiredSigningEnv("NAMO_RELEASE_PUBLIC_KEY"));
  const developerPrivatePath = resolve(requiredSigningEnv("NAMO_DEVELOPER_PRIVATE_KEY"));
  const passphrase = requiredSigningEnv("NAMO_DEVELOPER_KEY_PASSPHRASE");
  const trustRevision = Number(requiredSigningEnv("NAMO_TRUST_REVISION"));
  const securityEpoch = Number(version.firmwareSecurityEpoch);
  if (!Number.isSafeInteger(trustRevision) || trustRevision < 1 || trustRevision > 0xffffffff)
    fail("NAMO_TRUST_REVISION must be a positive uint32");
  if (!Number.isSafeInteger(securityEpoch) || securityEpoch < 1 || securityEpoch > 0xffffffff)
    fail("version.json firmwareSecurityEpoch must be a positive uint32");
  for (const path of [rootPublicPath, releasePublicPath, developerPrivatePath]) {
    if (!existsSync(path)) fail(`signing key file not found: ${path}`);
  }
  const rootPublic = loadPublicKey(readFileSync(rootPublicPath));
  const releasePublic = loadPublicKey(readFileSync(releasePublicPath));
  const developerPrivate = loadPrivateKey(
    readFileSync(developerPrivatePath), passphrase
  );
  const developerPublic = createPublicKey(developerPrivate);
  assertFirmwareSigningIdentity({
    role: "developer",
    privateKey: developerPrivate,
    rootPublicKey: rootPublic,
    releasePublicKey: releasePublic,
    developerPublicKey: developerPublic
  });
  return {
    rootPublic,
    releasePublic,
    developerPrivate,
    developerPublic,
    trustRevision,
    securityEpoch,
    buildId
  };
}

function provisionFirmwareTrustHeader(context) {
  const text = `#pragma once

#include <array>
#include <cstdint>

namespace esphome {
namespace radar_api_server {
namespace firmware_trust_keys {

static constexpr bool PROVISIONED = true;
static constexpr uint32_t TRUST_REVISION = ${context.trustRevision}U;
static constexpr uint32_t SECURITY_EPOCH = ${context.securityEpoch}U;
static constexpr std::array<uint8_t, 16> ROOT_KEY_ID${cppBytes(p256KeyId(context.rootPublic))};
static constexpr std::array<uint8_t, 65> ROOT_PUBLIC_KEY${cppBytes(rawP256PublicKey(context.rootPublic))};
static constexpr std::array<uint8_t, 16> RELEASE_KEY_ID${cppBytes(p256KeyId(context.releasePublic))};
static constexpr std::array<uint8_t, 65> RELEASE_PUBLIC_KEY${cppBytes(rawP256PublicKey(context.releasePublic))};
static constexpr std::array<uint8_t, 16> DEVELOPER_KEY_ID${cppBytes(p256KeyId(context.developerPublic))};
static constexpr std::array<uint8_t, 65> DEVELOPER_PUBLIC_KEY${cppBytes(rawP256PublicKey(context.developerPublic))};

}  // namespace firmware_trust_keys
}  // namespace radar_api_server
}  // namespace esphome
`;
  writeTextFileSync(firmwareTrustKeysPath, text);
}

function syncDashboardAssetVersions(version) {
  const current = readTextIfExists(dashboardAssetsPath);
  if (current == null) {
    return;
  }
  const firmwareVersion = String(version.firmware ?? "").trim();
  if (!firmwareVersion) fail("version.json firmware is empty");
  let next;
  let dashboardVersion;
  try {
    next = syncEmbeddedFirmwareVersion(current, firmwareVersion);
    dashboardVersion = embeddedDashboardVersion(current);
  } catch (error) {
    fail(error instanceof Error ? error.message : String(error));
  }
  if (next !== current) {
    writeTextFileSync(dashboardAssetsPath, next);
    console.log(`compile-firmware: synced embedded firmware=${firmwareVersion}; dashboard payload remains ${dashboardVersion}`);
  }
}

function commandFor(command, args) {
  if (process.platform === "win32") {
    return [process.env.ComSpec || "cmd.exe", ["/d", "/s", "/c", [command, ...args].join(" ")]];
  }
  return [command, args];
}

function runCommand(label, command, args, cwd, environment = null) {
  console.log(`\n${label}`);
  const [runner, runnerArgs] = commandFor(command, args);
  const result = spawnSync(runner, runnerArgs, {
    cwd,
    stdio: "inherit",
    env: environment == null ? process.env : { ...process.env, ...environment }
  });
  if (result.status !== 0) {
    throw new Error(`${label} failed with exit code ${result.status}`);
  }
}

function runEsphome(subcommand, firmwareEntry, transportSmoke = false, productionFusion = false, signedOta = false) {
  const substitutions = firmwareSubstitutions({ transportSmoke, productionFusion, signedOta });
  const esphomePython = process.env.ESPHOME_PYTHON;
  if (esphomePython) {
    runCommand(`esphome ${subcommand}`, esphomePython, ["-m", "esphome", ...substitutions, subcommand, firmwareEntry], repoRoot);
    return;
  }

  const pythonCandidates = [];
  if (process.env.VIRTUAL_ENV) {
    pythonCandidates.push(
      resolve(process.env.VIRTUAL_ENV, process.platform === "win32" ? "Scripts/python.exe" : "bin/python")
    );
  }
  pythonCandidates.push(
    resolve(repoRoot, ".venv", process.platform === "win32" ? "Scripts/python.exe" : "bin/python")
  );
  for (const python of pythonCandidates) {
    if (existsSync(python)) {
      runCommand(`esphome ${subcommand}`, python, ["-m", "esphome", ...substitutions, subcommand, firmwareEntry], repoRoot);
      return;
    }
  }

  runCommand(`esphome ${subcommand}`, "esphome", [...substitutions, subcommand, firmwareEntry], repoRoot);
}

function runEsphomeClean(firmwareEntry, transportSmoke, productionFusion, signedOta) {
  runEsphome("clean", firmwareEntry, transportSmoke, productionFusion, signedOta);
  const buildEnvironment = resolve(
    repoRoot,
    ".esphome",
    "build",
    defaultBuildEnvName,
    ".pioenvs",
    defaultBuildEnvName
  );
  rmSync(buildEnvironment, { recursive: true, force: true });
  console.log("compile-firmware: removed cached ESP-IDF build graph");
}

function runEsphomeConfig(firmwareEntry, transportSmoke, productionFusion, signedOta) {
  runEsphome("config", firmwareEntry, transportSmoke, productionFusion, signedOta);
}

function runEsphomeCompile(firmwareEntry, transportSmoke, productionFusion, signedOta) {
  runEsphome("compile", firmwareEntry, transportSmoke, productionFusion, signedOta);
}

function releaseEnvPaths() {
  const pioenvsDir = resolve(repoRoot, ".esphome", "build", defaultBuildEnvName, ".pioenvs");
  return {
    source: resolve(pioenvsDir, defaultBuildEnvName),
    target: resolve(pioenvsDir, releaseBuildEnvName)
  };
}

function moveReleaseBuildEnv() {
  const { source, target } = releaseEnvPaths();
  if (!existsSync(source)) {
    throw new Error(`release build output not found: ${source}`);
  }
  rmSync(target, { recursive: true, force: true });
  renameSync(source, target);
  console.log(`compile-firmware: release build output moved to ${target}`);
  return target;
}

function copyReleaseImage(envDir, sourceName, targetName) {
  const source = resolve(envDir, sourceName);
  const target = resolve(envDir, targetName);
  if (!existsSync(source)) {
    throw new Error(`release image not found: ${source}`);
  }
  copyFileSync(source, target);
  console.log(`compile-firmware: wrote ${targetName}`);
}

function writeReleaseImages(firmwareVersion, signing) {
  const envDir = moveReleaseBuildEnv();
  const rawFirmware = readFileSync(resolve(envDir, "firmware.ota.bin"));
  assertSignedOtaFirmwareImage(rawFirmware);
  assertFirmwareEmbedsPublicKeys(rawFirmware, [
    signing.rootPublic,
    signing.releasePublic,
    signing.developerPublic
  ]);
  const embeddedVersion = readEspFirmwareVersion(rawFirmware);
  if (embeddedVersion !== firmwareVersion) {
    throw new Error(
      `release image version mismatch: expected ${firmwareVersion}, got ${embeddedVersion}`
    );
  }
  const signedPackage = createFirmwarePackage({
    firmware: rawFirmware,
    privateKey: signing.releasePrivate,
    role: "release",
    firmwareVersion,
    buildId: signing.buildId,
    trustRevision: signing.trustRevision,
    securityEpoch: signing.securityEpoch
  });
  verifyFirmwarePackage(signedPackage, signing.releasePublic);
  const packageName = `firmware-release-${firmwareVersion}.namo-ota`;
  const packagePath = resolve(envDir, packageName);
  writeFileSync(packagePath, signedPackage, { flag: "wx" });
  writeTextFileSync(
    `${packagePath}.sha256`,
    `${createHash("sha256").update(signedPackage).digest("hex")}  ${packageName}\n`
  );
  console.log(`compile-firmware: wrote ${packageName}`);
  copyReleaseImage(envDir, "firmware.factory.bin", `firmware.factory-release-${firmwareVersion}.bin`);
}

function writeDeveloperImages(firmwareVersion, signing) {
  const envDir = releaseEnvPaths().source;
  const rawFirmware = readFileSync(resolve(envDir, "firmware.ota.bin"));
  assertSignedOtaFirmwareImage(rawFirmware);
  assertFirmwareEmbedsPublicKeys(rawFirmware, [
    signing.rootPublic,
    signing.releasePublic,
    signing.developerPublic
  ]);
  const embeddedVersion = readEspFirmwareVersion(rawFirmware);
  if (embeddedVersion !== firmwareVersion) {
    throw new Error(
      `developer image version mismatch: expected ${firmwareVersion}, got ${embeddedVersion}`
    );
  }
  const signedPackage = createFirmwarePackage({
    firmware: rawFirmware,
    privateKey: signing.developerPrivate,
    role: "developer",
    firmwareVersion,
    buildId: signing.buildId,
    trustRevision: signing.trustRevision,
    securityEpoch: signing.securityEpoch
  });
  verifyFirmwarePackage(signedPackage, signing.developerPublic);
  const packageName = `firmware-developer-${firmwareVersion}.namo-ota`;
  const packagePath = resolve(envDir, packageName);
  writeFileSync(packagePath, signedPackage, { flag: "wx" });
  writeTextFileSync(
    `${packagePath}.sha256`,
    `${createHash("sha256").update(signedPackage).digest("hex")}  ${packageName}\n`
  );
  console.log(`compile-firmware: wrote ${packageName}`);
}

function writeBetaImages(firmwareVersion) {
  const envDir = releaseEnvPaths().source;
  copyReleaseImage(envDir, "firmware.ota.bin", `firmware.ota-beta-${firmwareVersion}.bin`);
  copyReleaseImage(envDir, "firmware.factory.bin", `firmware.factory-beta-${firmwareVersion}.bin`);
}

const options = parseArgs();
if (options.help) {
  usage();
  process.exit(0);
}

preparePlatformioEnvironment();

const snapshot = {
  versionText: readFileSync(versionPath, "utf8"),
  firmwareYamlText: readFileSync(firmwareYamlPath, "utf8"),
  dashboardAssetsText: readTextIfExists(dashboardAssetsPath),
  firmwareTrustKeysText: readFileSync(firmwareTrustKeysPath, "utf8")
};

const version = JSON.parse(snapshot.versionText);
const currentFirmware = String(version.firmware ?? "").trim();
if (!currentFirmware) {
  fail("version.json firmware is empty");
}

const currentDashboard = String(version.dashboard ?? "").trim();
if (!currentDashboard) {
  fail("version.json dashboard is empty");
}

const nextFirmware = nextFirmwareVersion(
  currentFirmware,
  options.release,
  options.releaseType
);
const nextDashboard = options.release
  ? nextReleaseVersion(currentDashboard, options.releaseType)
  : currentDashboard;
version.firmware = nextFirmware;
version.dashboard = nextDashboard;
version.firmwareSecurityEpoch = nextFirmwareSecurityEpoch(
  version.firmwareSecurityEpoch,
  options.release
);
writeTextFileSync(versionPath, `${JSON.stringify(version, null, 2)}\n`);

const buildStartedAt = new Date();
const releaseBuildId = buildStartedAt.toISOString().replace(/[-:]/g, "").replace(/\.\d{3}Z$/, "Z");
let signing = null;
console.log(`compile-firmware: firmware ${currentFirmware} -> ${nextFirmware}${options.hotfix ? " (hotfix release)" : options.release ? ` (${options.releaseType} release)` : options.signer === "developer" ? " (signed developer)" : options.beta ? " (beta)" : ""}`);
if (options.release) {
  console.log(`compile-firmware: dashboard ${currentDashboard} -> ${nextDashboard}`);
}
console.log(`compile-firmware: entry=${options.firmwareEntry}`);
if (options.transportSmoke) {
  console.warn("compile-firmware: WARNING transport smoke key enabled; do not distribute this image");
}
if (options.productionFusion) {
  console.log("compile-firmware: authenticated production fusion enabled");
}
console.log(`compile-firmware: started_at=${formatLocalDateTime(buildStartedAt)}`);

try {
  signing = options.signer === "release"
    ? releaseSigningContext(version, releaseBuildId)
    : options.signer === "developer"
      ? developerSigningContext(version, releaseBuildId)
      : null;
  if (signing) provisionFirmwareTrustHeader(signing);
  runCommand(
    "generate setup page translations",
    "node",
    ["dashboard/scripts/generate-setup-page-i18n.mjs"],
    repoRoot
  );
  runCommand("sync firmware version", "node", ["scripts/sync-version.mjs"], repoRoot);
  syncDashboardAssetVersions(version);
  if (options.dashboard || options.release || options.signer !== "none") {
    runCommand(
      "build embedded dashboard",
      "npm",
      ["run", "build:dashboard"],
      dashboardDir,
      options.release ? { NAMO_DASHBOARD_VERSION_PREBUMPED: "1" } : null
    );
  }
  if (firmwareBuildRequiresClean(options)) {
    runEsphomeClean(options.firmwareEntry, options.transportSmoke, options.productionFusion, options.signer !== "none");
  }
  if (options.config || options.release || options.signer !== "none") {
    runEsphomeConfig(options.firmwareEntry, options.transportSmoke, options.productionFusion, options.signer !== "none");
  }
  runEsphomeCompile(options.firmwareEntry, options.transportSmoke, options.productionFusion, options.signer !== "none");
  if (options.beta) {
    writeBetaImages(nextFirmware);
  } else if (options.release) {
    writeReleaseImages(nextFirmware, signing);
    runCommand("sync README release version", "node", ["scripts/sync-version.mjs", "--readme"], repoRoot);
  } else if (options.signer === "developer") {
    writeDeveloperImages(nextFirmware, signing);
  }
  const buildCompletedAt = new Date();
  console.log(`\ncompile-firmware: complete firmware=${nextFirmware}`);
  console.log(`compile-firmware: completed_at=${formatLocalDateTime(buildCompletedAt)} duration=${formatDuration(buildCompletedAt - buildStartedAt)}`);
  if (shouldRestoreFirmwareTrustHeaderAfterSuccess(options)) {
    writeTextFileSync(firmwareTrustKeysPath, snapshot.firmwareTrustKeysText);
  } else {
    assertFirmwareTrustEpochMatches(
      version,
      readFileSync(firmwareTrustKeysPath, "utf8")
    );
    console.log(
      `compile-firmware: retained release trust header security_epoch=${version.firmwareSecurityEpoch}`
    );
  }
} catch (error) {
  const buildFailedAt = new Date();
  console.error(`\ncompile-firmware: ${error instanceof Error ? error.message : String(error)}`);
  console.error(`compile-firmware: failed_at=${formatLocalDateTime(buildFailedAt)} duration=${formatDuration(buildFailedAt - buildStartedAt)}`);
  console.error("compile-firmware: restoring version files");
  restore(snapshot);
  process.exit(1);
}

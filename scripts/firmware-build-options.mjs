export function firmwareSubstitutions({
  transportSmoke = false,
  productionFusion = true,
  signedOta = false
} = {}) {
  if (transportSmoke && productionFusion) {
    throw new Error("transport smoke and production fusion are mutually exclusive");
  }

  const substitutions = [];
  if (transportSmoke) {
    substitutions.push("-s", "shadow_transport_smoke", "true");
  }
  substitutions.push("-s", "production_fusion", productionFusion ? "true" : "false");
  substitutions.push("-s", "signed_ota", signedOta ? "true" : "false");
  substitutions.push("-s", "ota_policy", signedOta ? "signed" : "legacy");
  return substitutions;
}

export function nextFirmwareSecurityEpoch(current, release = false) {
  const value = Number(current);
  if (!Number.isSafeInteger(value) || value < 1 || value > 0xffffffff) {
    throw new Error("firmware security epoch must be a positive uint32");
  }
  if (!release) return value;
  if (value === 0xffffffff) {
    throw new Error("firmware security epoch is exhausted");
  }
  return value + 1;
}

export function firmwareBuildRequiresClean({ clean = false, release = false } = {}) {
  return Boolean(clean || release);
}

export function shouldRestoreFirmwareTrustHeaderAfterSuccess({ release = false } = {}) {
  return !release;
}

export function assertFirmwareTrustEpochMatches(version, trustHeader) {
  const expected = Number(version?.firmwareSecurityEpoch);
  if (!Number.isSafeInteger(expected) || expected < 1 || expected > 0xffffffff) {
    throw new Error("version.json firmwareSecurityEpoch must be a positive uint32");
  }
  const match = /SECURITY_EPOCH\s*=\s*(\d+)U/.exec(String(trustHeader ?? ""));
  if (!match) {
    throw new Error("firmware trust header SECURITY_EPOCH is missing");
  }
  const actual = Number(match[1]);
  if (actual !== expected) {
    throw new Error(
      `firmware trust epoch mismatch: version.json=${expected} firmware_trust_keys.h=${actual}`
    );
  }
  return actual;
}

export function parseFirmwareBuildArgs(args, {
  defaultFirmwareEntry = "namo.yaml"
} = {}) {
  if (!Array.isArray(args)) {
    throw new Error("firmware build arguments must be an array");
  }

  const values = [];
  let explicitSigner = null;
  let explicitReleaseType = null;
  for (let index = 0; index < args.length; index += 1) {
    const argument = args[index];
    if (argument !== "--signer" && argument !== "--release-type") {
      values.push(argument);
      continue;
    }
    const optionValue = args[index + 1];
    if (!optionValue || optionValue.startsWith("--")) {
      throw new Error(
        argument === "--signer"
          ? "--signer requires none, developer, or release"
          : "--release-type requires patch, minor, or major"
      );
    }
    if (argument === "--signer") {
      if (explicitSigner !== null) {
        throw new Error("--signer may only be provided once");
      }
      if (!new Set(["none", "developer", "release"]).has(optionValue)) {
        throw new Error(`unsupported signer: ${optionValue}`);
      }
      explicitSigner = optionValue;
    } else {
      if (explicitReleaseType !== null) {
        throw new Error("--release-type may only be provided once");
      }
      const releaseType = optionValue.toLowerCase();
      if (!new Set(["patch", "minor", "major"]).has(releaseType)) {
        throw new Error(`unsupported release type: ${optionValue}`);
      }
      explicitReleaseType = releaseType;
    }
    index += 1;
  }

  const beta = values.includes("--beta");
  const release = values.includes("--release");
  const signedDeveloperAlias = values.includes("--signed-developer");
  const dashboard = values.includes("--dashboard");
  const clean = values.includes("--clean") || values.includes("--cmake-clean");
  const config = values.includes("--config");
  const transportSmoke = values.includes("--transport-smoke");
  const productionFusionRequested = values.includes("--production-fusion");
  const help = values.includes("--help") || values.includes("-h");
  const knownOptions = new Set([
    "--beta",
    "--release",
    "--signed-developer",
    "--dashboard",
    "--clean",
    "--cmake-clean",
    "--config",
    "--transport-smoke",
    "--production-fusion",
    "--help",
    "-h"
  ]);
  const unknown = values.filter(
    (argument) => argument.startsWith("--") && !knownOptions.has(argument)
  );
  if (unknown.length) {
    throw new Error(`unknown option: ${unknown.join(", ")}`);
  }
  const positional = values.filter((argument) => !argument.startsWith("--"));
  if (positional.length > 1) {
    throw new Error("only one firmware yaml path can be provided");
  }
  if (beta && release) {
    throw new Error("--beta and --release are mutually exclusive");
  }
  if (signedDeveloperAlias && explicitSigner !== null) {
    throw new Error("--signed-developer cannot be combined with --signer");
  }

  const signer = signedDeveloperAlias ? "developer" : explicitSigner ?? "none";
  if (release && signer !== "release") {
    throw new Error("--release requires the separate --signer release option");
  }
  if (!release && signer === "release") {
    throw new Error("--signer release requires --release");
  }
  if (!release && explicitReleaseType !== null) {
    throw new Error("--release-type requires --release");
  }
  if (beta && signer !== "none") {
    throw new Error("--beta cannot be combined with a signed build");
  }
  if (transportSmoke && (beta || release || signer !== "none")) {
    throw new Error("--transport-smoke cannot be combined with signed or export builds");
  }
  if (transportSmoke && productionFusionRequested) {
    throw new Error("--transport-smoke cannot be combined with --production-fusion");
  }

  return {
    beta,
    clean,
    config,
    dashboard,
    help,
    release,
    signer,
    transportSmoke,
    productionFusion: !transportSmoke,
    releaseType: explicitReleaseType ?? "patch",
    firmwareEntry: positional[0] ?? defaultFirmwareEntry
  };
}

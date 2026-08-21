import {
  PlatformPairingError,
  type PlatformSiteConfigurationStatus
} from "../api/platform-pairing";

interface SharedFloorplanWriteWaitOptions {
  readSite: () => Promise<PlatformSiteConfigurationStatus>;
  sleep?: (milliseconds: number) => Promise<void>;
  timeoutMs?: number;
  pollIntervalMs?: number;
  onWaiting?: (site: PlatformSiteConfigurationStatus) => void;
}

interface SharedFloorplanMutationOptions<T> extends SharedFloorplanWriteWaitOptions {
  mutation: () => Promise<T>;
}

const DEFAULT_TIMEOUT_MS = 60000;
const DEFAULT_POLL_INTERVAL_MS = 500;

export async function waitForSharedFloorplanWritable({
  readSite,
  sleep = defaultSleep,
  timeoutMs = DEFAULT_TIMEOUT_MS,
  pollIntervalMs = DEFAULT_POLL_INTERVAL_MS,
  onWaiting
}: SharedFloorplanWriteWaitOptions): Promise<PlatformSiteConfigurationStatus> {
  const deadline = Date.now() + Math.max(0, timeoutMs);
  let lastSite: PlatformSiteConfigurationStatus | undefined;
  while (true) {
    try {
      const site = await readSite();
      lastSite = site;
      assertSharedFloorplanOwner(site);
      const conflict = site.configurationConsistent === false;
      if (!conflict && site.sharedWritesAllowed !== false) return site;
      onWaiting?.(site);
    } catch (cause) {
      if (!sharedFloorplanReadCanRetry(cause)) throw cause;
    }
    if (Date.now() >= deadline) {
      if (lastSite?.configurationConsistent === false) {
        throw new PlatformPairingError(
          "site_configuration_conflict",
          409,
          "site_configuration_conflict"
        );
      }
      throw new PlatformPairingError(
        "site_configuration_continuity_unavailable",
        409,
        "site_configuration_continuity_unavailable"
      );
    }
    await sleep(Math.max(1, pollIntervalMs));
  }
}

export async function runSharedFloorplanMutationAfterContinuity<T>({
  mutation,
  ...waitOptions
}: SharedFloorplanMutationOptions<T>): Promise<T> {
  await waitForSharedFloorplanWritable(waitOptions);
  try {
    return await mutation();
  } catch (cause) {
    if (!(cause instanceof PlatformPairingError) ||
        (cause.code !== "site_configuration_continuity_unavailable" &&
         cause.code !== "site_configuration_conflict")) {
      throw cause;
    }
    await waitForSharedFloorplanWritable(waitOptions);
    return mutation();
  }
}

function assertSharedFloorplanOwner(site: PlatformSiteConfigurationStatus): void {
  if (site.configurationOwnerNodeId && !site.localOwner) {
    throw new PlatformPairingError(
      "site_configuration_owner_required",
      403,
      "site_configuration_owner_required"
    );
  }
}

function sharedFloorplanReadCanRetry(cause: unknown): boolean {
  return cause instanceof TypeError ||
    (cause instanceof PlatformPairingError &&
      (cause.status === 0 || cause.status === 408 || cause.status === 429 ||
       cause.status >= 500));
}

function defaultSleep(milliseconds: number): Promise<void> {
  return new Promise((resolve) => globalThis.setTimeout(resolve, milliseconds));
}

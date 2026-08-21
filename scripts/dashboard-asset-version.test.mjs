import assert from "node:assert/strict";
import test from "node:test";

import {
  embeddedDashboardVersion,
  syncEmbeddedFirmwareVersion
} from "./dashboard-asset-version.mjs";

const fixture = `inline constexpr const char *FIRMWARE_VERSION = "0.5.4";
inline constexpr const char *DASHBOARD_VERSION = "0.4.3";`;

test("normal firmware builds preserve the version of the embedded dashboard payload", () => {
  const updated = syncEmbeddedFirmwareVersion(fixture, "0.5.5-a");
  assert.match(updated, /FIRMWARE_VERSION = "0\.5\.5-a"/);
  assert.equal(embeddedDashboardVersion(updated), "0.4.3");
});

test("missing embedded version markers fail instead of producing misleading metadata", () => {
  assert.throws(() => syncEmbeddedFirmwareVersion("", "0.5.5-a"), /marker is missing/);
  assert.throws(() => embeddedDashboardVersion(""), /marker is missing/);
});

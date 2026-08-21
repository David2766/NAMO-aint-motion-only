import { readFileSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { nextDashboardBuildVersion } from "./release-version.mjs";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const versionPath = resolve(repoRoot, "version.json");

function fail(message) {
  console.error(`bump-dashboard-version: ${message}`);
  process.exit(1);
}

if (process.argv.length > 2) {
  fail("this script does not accept arguments");
}

const version = JSON.parse(readFileSync(versionPath, "utf8"));
const current = String(version.dashboard ?? "").trim();

if (!current) {
  fail("version.json dashboard is empty");
}

let next;
try {
  next = nextDashboardBuildVersion(current, {
    releasePrebumped: process.env.NAMO_DASHBOARD_VERSION_PREBUMPED === "1"
  });
} catch (error) {
  fail(error instanceof Error ? error.message : String(error));
}
version.dashboard = next;

writeFileSync(versionPath, `${JSON.stringify(version, null, 2)}\n`, "utf8");

console.log(
  next === current
    ? `bump-dashboard-version: preserving release version ${current}`
    : `bump-dashboard-version: ${current} -> ${next}`
);

import assert from "node:assert/strict";
import test from "node:test";

import {
  nextDashboardBuildVersion,
  nextReleaseVersion,
  normalizeReleaseType,
  parseSemanticVersion
} from "./release-version.mjs";

test("release types advance semantic version segments and clear lower segments", () => {
  assert.equal(nextReleaseVersion("0.5.5-o", "patch"), "0.5.6");
  assert.equal(nextReleaseVersion("0.5.5-o", "minor"), "0.6.0");
  assert.equal(nextReleaseVersion("0.5.5-o", "major"), "1.0.0");
  assert.equal(nextReleaseVersion("0.9.8", "minor"), "0.10.0");
  assert.equal(nextReleaseVersion("2.7.9-z", "major"), "3.0.0");
});

test("dashboard builds preserve a release version only when it was pre-bumped", () => {
  assert.equal(
    nextDashboardBuildVersion("0.5.0", { releasePrebumped: true }),
    "0.5.0"
  );
  assert.equal(nextDashboardBuildVersion("0.5.0"), "0.5.0-a");
  assert.equal(nextDashboardBuildVersion("0.4.13-k"), "0.4.13-l");
  assert.equal(nextDashboardBuildVersion("0.4.13-z"), "0.4.14-a");
});

test("release version parsing remains bounded to the shipped version format", () => {
  assert.deepEqual(parseSemanticVersion("0.4.13-k"), {
    major: 0,
    minor: 4,
    patch: 13,
    suffix: "k"
  });
  assert.equal(normalizeReleaseType("Minor"), "minor");
  assert.throws(() => nextReleaseVersion("0.5", "patch"), /unsupported semantic version/);
  assert.throws(() => nextReleaseVersion("0.5.5", "feature"), /unsupported release type/);
});

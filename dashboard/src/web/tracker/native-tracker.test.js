import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

const repoRoot = fileURLToPath(new URL("../../../../", import.meta.url));
const testScript = fileURLToPath(new URL("../../../../tools/presence-replay/native/test.py", import.meta.url));
const compilerProbe = spawnSync("python", [testScript, "--check-compiler"], {
  cwd: repoRoot,
  encoding: "utf8",
  timeout: 30_000
});
const compilerMissing =
  compilerProbe.status === 2 && compilerProbe.stdout.includes("tracker_tests: skipped (no host C++ compiler found)");
const nativeTest = compilerMissing ? it.skip : it;

describe("native PresenceTracker", () => {
  nativeTest("passes C++ room-exit lifecycle tests", () => {
    if (compilerProbe.status !== 0) {
      throw new Error(`${compilerProbe.stdout}\n${compilerProbe.stderr}`);
    }
    const result = spawnSync("python", [testScript], {
      cwd: repoRoot,
      encoding: "utf8",
      timeout: 240_000
    });

    expect(result.status, `${result.stdout}\n${result.stderr}`).toBe(0);
    expect(result.stdout).toContain("tracker_tests: ok");
  }, 240_000);
});

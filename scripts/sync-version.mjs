import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { writeTextFileSync } from "./reliable-write.mjs";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const versionPath = resolve(repoRoot, "version.json");
const firmwareYamlPath = resolve(repoRoot, "namo.yaml");
const readmePath = resolve(repoRoot, "README.md");
const args = process.argv.slice(2);
const syncReadme = args.includes("--readme");
const unknownArgs = args.filter((arg) => arg !== "--readme");

function fail(message) {
  console.error(`sync-version: ${message}`);
  process.exit(1);
}

if (unknownArgs.length > 0) {
  fail(`unknown option: ${unknownArgs.join(", ")}`);
}

const version = JSON.parse(readFileSync(versionPath, "utf8"));
const firmwareVersion = String(version.firmware ?? "").trim();

if (!firmwareVersion) {
  fail("version.json firmware is empty");
}

const yaml = readFileSync(firmwareYamlPath, "utf8");
const nextYaml = yaml.replace(
  /^(\s*project_version:\s*)["']?[^"'\r\n]+["']?/m,
  `$1"${firmwareVersion}"`
);

if (nextYaml === yaml && !yaml.includes(`project_version: "${firmwareVersion}"`)) {
  fail("project_version substitution not found");
}

let nextReadme = null;
if (syncReadme) {
  const readme = readFileSync(readmePath, "utf8");
  const versionPattern = /(<!-- release-firmware-version -->\s*NAMO v)\d+\.\d+\.\d+(?:-[a-z])?/g;
  const versionReferences = [...readme.matchAll(versionPattern)];
  if (versionReferences.length !== 2) {
    fail(`expected exactly 2 README firmware version references, found ${versionReferences.length}`);
  }
  nextReadme = readme.replace(versionPattern, (_match, prefix) => `${prefix}${firmwareVersion}`);
}

writeTextFileSync(firmwareYamlPath, nextYaml);
if (nextReadme != null) {
  writeTextFileSync(readmePath, nextReadme);
}

console.log(`sync-version: firmware=${firmwareVersion}${syncReadme ? " readme=updated" : ""}`);

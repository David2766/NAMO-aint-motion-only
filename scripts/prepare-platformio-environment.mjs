import { existsSync, readFileSync } from "node:fs";
import { homedir } from "node:os";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { writeTextFileSync } from "./reliable-write.mjs";

const aliasBlock = [
  "    # pioarduino-core is the distribution name used by the forked PlatformIO core.",
  "    # Treat it as satisfying the platformio dependency to avoid downloading the",
  "    # same pinned core archive on every build.",
  "    if \"platformio\" not in installed_packages and \"pioarduino-core\" in installed_packages:",
  "        installed_packages[\"platformio\"] = installed_packages[\"pioarduino-core\"]",
  ""
].join("\n");

const insertionPoint = "    installed_packages = _get_installed_uv_packages()\n";

export function platformioCoreDirectory(environment = process.env) {
  const configured = String(environment.PLATFORMIO_CORE_DIR ?? "").trim();
  return configured ? resolve(configured) : resolve(homedir(), ".platformio");
}

export function pioarduinoPenvSetupPath(environment = process.env) {
  return resolve(
    platformioCoreDirectory(environment),
    "platforms",
    "espressif32",
    "builder",
    "penv_setup.py"
  );
}

export function patchPioarduinoDependencyAlias(source) {
  if (source.includes('installed_packages["platformio"] = installed_packages["pioarduino-core"]')) {
    return { changed: false, source };
  }

  const first = source.indexOf(insertionPoint);
  const last = source.lastIndexOf(insertionPoint);
  if (first < 0 || first !== last) {
    throw new Error("unsupported pioarduino penv dependency-check layout");
  }

  const offset = first + insertionPoint.length;
  return {
    changed: true,
    source: `${source.slice(0, offset)}${aliasBlock}${source.slice(offset)}`
  };
}

export function preparePlatformioEnvironment({ environment = process.env, log = console.log } = {}) {
  const path = pioarduinoPenvSetupPath(environment);
  if (!existsSync(path)) {
    log(`compile-firmware: pioarduino dependency check not present; skipped (${path})`);
    return { changed: false, path, skipped: true };
  }

  const current = readFileSync(path, "utf8");
  const patched = patchPioarduinoDependencyAlias(current);
  if (patched.changed) {
    writeTextFileSync(path, patched.source);
    log("compile-firmware: corrected pioarduino PlatformIO dependency alias check");
  }
  return { changed: patched.changed, path, skipped: false };
}

const invokedPath = process.argv[1] ? resolve(process.argv[1]) : "";
if (invokedPath === fileURLToPath(import.meta.url)) {
  preparePlatformioEnvironment();
}

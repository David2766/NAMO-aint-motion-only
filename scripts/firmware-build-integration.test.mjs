import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import test from "node:test";

const root = resolve(import.meta.dirname, "..");
const read = (path) => readFileSync(resolve(root, path), "utf8");

test("ESP-IDF sources use the framework mbedTLS configuration", () => {
  const component = read("components/radar_api_server/__init__.py");
  const replication = read(
    "components/radar_api_server/site_replication.cpp"
  );

  assert.match(
    component,
    /-DMBEDTLS_CONFIG_FILE=\\\\"mbedtls\/esp_config\.h\\\\"/
  );
  assert.match(replication, /!defined\(MBEDTLS_AES_ALT\)/);
  assert.match(
    replication,
    /#error "ESP-IDF mbedTLS AES configuration is required"/
  );
});

test("firmware clean invalidates the cached ESP-IDF source graph", () => {
  const compile = read("scripts/compile-firmware.mjs");
  const cleanStart = compile.indexOf("function runEsphomeClean(");
  const cleanEnd = compile.indexOf("function runEsphomeConfig(", cleanStart);
  const clean = compile.slice(cleanStart, cleanEnd);

  assert.ok(cleanStart >= 0 && cleanEnd > cleanStart);
  assert.match(clean, /runEsphome\("clean"/);
  assert.match(
    clean,
    /"\.pioenvs",\s*defaultBuildEnvName[\s\S]*rmSync\(buildEnvironment, \{ recursive: true, force: true \}\)/
  );
});

test("firmware builds correct the pioarduino dependency alias before ESPHome runs", () => {
  const compile = read("scripts/compile-firmware.mjs");
  const prepare = compile.indexOf("preparePlatformioEnvironment();");
  const snapshot = compile.indexOf("const snapshot = {");
  const compileCall = compile.indexOf("runEsphomeCompile(", snapshot);

  assert.ok(prepare >= 0);
  assert.ok(prepare < snapshot);
  assert.ok(prepare < compileCall);
});

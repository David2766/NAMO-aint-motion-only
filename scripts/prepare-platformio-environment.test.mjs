import assert from "node:assert/strict";
import test from "node:test";

import { patchPioarduinoDependencyAlias } from "./prepare-platformio-environment.mjs";

const upstream = `def install_python_deps():
    installed_packages = _get_installed_uv_packages()
    packages_to_install = list(get_packages_to_install(python_deps, installed_packages))
`;

test("pioarduino core distribution satisfies the PlatformIO dependency", () => {
  const result = patchPioarduinoDependencyAlias(upstream);

  assert.equal(result.changed, true);
  assert.match(
    result.source,
    /if "platformio" not in installed_packages and "pioarduino-core" in installed_packages:/
  );
  assert.ok(
    result.source.indexOf('installed_packages["platformio"]') <
      result.source.indexOf("packages_to_install =")
  );
});

test("pioarduino dependency correction is idempotent", () => {
  const first = patchPioarduinoDependencyAlias(upstream);
  const second = patchPioarduinoDependencyAlias(first.source);

  assert.equal(second.changed, false);
  assert.equal(second.source, first.source);
});

test("unknown upstream layouts fail instead of editing the wrong location", () => {
  assert.throws(
    () => patchPioarduinoDependencyAlias("def install_python_deps():\n    pass\n"),
    /unsupported pioarduino penv dependency-check layout/
  );
});

import assert from "node:assert/strict";
import { test } from "node:test";

import { writeTextFileSync } from "./reliable-write.mjs";

test("retries transient Windows file-open failures", () => {
  let writes = 0;
  let waits = 0;
  writeTextFileSync("namo.yaml", "content", {
    write: (_path, _contents, encoding) => {
      writes += 1;
      assert.equal(encoding, "utf8");
      if (writes < 3) {
        const error = new Error("temporarily locked");
        error.code = "UNKNOWN";
        throw error;
      }
    },
    wait: () => {
      waits += 1;
    }
  });

  assert.equal(writes, 3);
  assert.equal(waits, 2);
});

test("does not hide permanent write failures", () => {
  const failure = Object.assign(new Error("missing directory"), {
    code: "ENOENT"
  });
  assert.throws(
    () => writeTextFileSync("missing/file", "content", {
      write: () => {
        throw failure;
      },
      wait: () => assert.fail("permanent errors must not wait")
    }),
    failure
  );
});

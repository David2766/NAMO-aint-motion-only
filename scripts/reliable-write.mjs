import { writeFileSync } from "node:fs";

const RETRYABLE_CODES = new Set(["EACCES", "EBUSY", "EPERM", "UNKNOWN"]);

function sleepSync(milliseconds) {
  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, milliseconds);
}

export function writeTextFileSync(path, contents, options = {}) {
  const attempts = options.attempts ?? 20;
  const delayMs = options.delayMs ?? 100;
  const write = options.write ?? writeFileSync;
  const wait = options.wait ?? sleepSync;

  for (let attempt = 1; attempt <= attempts; attempt += 1) {
    try {
      write(path, contents, "utf8");
      return;
    } catch (error) {
      const retryable = error && RETRYABLE_CODES.has(error.code);
      if (!retryable || attempt === attempts) throw error;
      wait(delayMs);
    }
  }
}

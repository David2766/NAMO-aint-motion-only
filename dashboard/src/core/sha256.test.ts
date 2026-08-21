import { describe, expect, it } from "vitest";

import { sha256, sha256Hex } from "./sha256";

describe("sha256", () => {
  it.each([
    [0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"],
    [1, "4a64a107f0cb32536e5bce6c98c393db21cca7f4ea187ba8c4dca8b51d4ea80a"],
    [55, "59aaae80b8e7958f5757b4ac9274f1fd57ec0dc7aa8349319102316781b92006"],
    [56, "dca902d31487ffab357ce36cc5abc6947fbb69127ec04c468ba5687268c4bf7c"],
    [63, "b8e655e9e7ad413b96f0a371eb1d6db71dcca27f4eee35892517cbe4bfaa6f9a"],
    [64, "e5146be62accc56709594cb45c651b361df94f622cbb09b91ea3ca7a2060bd53"],
    [65, "4b6c6774f0cbcd776a90e187b9494e04ca880c0afe3ea1c5cad51c8bbcb41f74"],
    [1024, "575383deff0775797e1ff4c82da069141937c843a81cbe50959a96bcfa88140e"],
    [1024 * 1024, "ea53eb498f9b7dc054aae73924bf25c569604d2c20aa9016f0b8714a1c180c7b"]
  ] as const)(
    "matches the reference digest at the %i-byte padding boundary",
    (size, expected) => {
      const input = Uint8Array.from({ length: size }, (_, index) => (index * 131 + 17) & 0xff);
      expect(sha256Hex(input)).toBe(expected);
      expect([...sha256(input)].map((value) => value.toString(16).padStart(2, "0")).join(""))
        .toBe(expected);
    }
  );
});

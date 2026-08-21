import { describe, expect, it } from "vitest";
import { en } from "./en";
import { ko } from "./ko";

type MessageTree = Record<string, unknown>;

function messagePaths(value: unknown, prefix = ""): string[] {
  if (typeof value === "string" || typeof value === "function") return [prefix];
  if (!value || typeof value !== "object" || Array.isArray(value)) return [prefix];

  return Object.entries(value as MessageTree).flatMap(([key, child]) => {
    const path = prefix ? `${prefix}.${key}` : key;
    return messagePaths(child, path);
  });
}

function assertNoEmptyStrings(value: unknown, prefix = ""): void {
  if (typeof value === "string") {
    expect(value.trim(), prefix).not.toBe("");
    return;
  }
  if (!value || typeof value !== "object" || Array.isArray(value)) return;

  for (const [key, child] of Object.entries(value as MessageTree)) {
    assertNoEmptyStrings(child, prefix ? `${prefix}.${key}` : key);
  }
}

describe("i18n message dictionaries", () => {
  it("keeps Korean and English message trees in sync", () => {
    expect(messagePaths(ko).sort()).toEqual(messagePaths(en).sort());
  });

  it("does not contain empty string messages", () => {
    assertNoEmptyStrings(en);
    assertNoEmptyStrings(ko);
  });

  it("contains the core API error codes used by device and demo flows", () => {
    const requiredCodes = [
      "invalid_request",
      "config_not_found",
      "config_write_failed",
      "upload_session_mismatch",
      "upload_offset_mismatch",
      "upload_incomplete",
      "upload_payload_too_large",
      "upload_storage_failed",
      "api_key_unsupported",
      "nothing_selected",
      "wifi_not_ready",
      "wifi_unavailable",
      "open_wifi_unsupported",
      "invalid_password_length",
      "firmware_upload_failed",
      "firmware_network_error",
      "firmware_upload_aborted",
      "firmware_trust_unavailable",
      "firmware_header_invalid",
      "signed_firmware_required",
      "legacy_firmware_bin_required",
      "firmware_session_invalid",
      "busy",
      "session_mismatch",
      "header_mismatch",
      "trust_rejected",
      "offset_mismatch",
      "size_mismatch",
      "expired",
      "sink_begin_failed",
      "sink_write_failed",
      "sink_finish_failed"
    ];

    for (const code of requiredCodes) {
      expect(en.api.errors[code], `en.api.errors.${code}`).toBeTruthy();
      expect(ko.api.errors[code], `ko.api.errors.${code}`).toBeTruthy();
    }
  });

  it("contains the setup and native API status codes used by the contract migration", () => {
    const requiredCodes = [
      "setup_wifi_connecting",
      "setup_wifi_connected",
      "setup_wifi_failed",
      "ha_handoff_started",
      "api_client_waiting",
      "api_client_connected",
      "api_client_idle",
      "api_service_not_ready"
    ];

    for (const code of requiredCodes) {
      expect(en.api.statuses[code], `en.api.statuses.${code}`).toBeTruthy();
      expect(ko.api.statuses[code], `ko.api.statuses.${code}`).toBeTruthy();
    }
  });
});

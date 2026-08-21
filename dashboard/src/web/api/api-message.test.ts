import { describe, expect, it } from "vitest";
import { en } from "../i18n/en";
import { ko } from "../i18n/ko";
import { apiErrorCode, apiErrorMessage, apiStatusMessage, fallbackErrorMessage } from "./api-message";

describe("api message helpers", () => {
  it("prefers contract errorInfo codes", () => {
    expect(apiErrorCode({
      code: "legacy_code",
      errorInfo: {
        code: "upload_session_mismatch"
      }
    })).toBe("upload_session_mismatch");
  });

  it("translates known error codes in Korean and English", () => {
    expect(apiErrorMessage(ko, { errorInfo: { code: "upload_session_mismatch" } })).toContain("업로드");
    expect(apiErrorMessage(en, { errorInfo: { code: "upload_session_mismatch" } })).toContain("upload");
  });

  it("translates shared upload errors used by floorplan and stats restore", () => {
    expect(apiErrorMessage(ko, { errorInfo: { code: "upload_storage_failed" } })).toContain("저장");
    expect(apiErrorMessage(en, { errorInfo: { code: "upload_storage_failed" } })).toContain("write");
    expect(apiErrorMessage(ko, { errorInfo: { code: "upload_payload_too_large" } })).toContain("초과");
    expect(apiErrorMessage(en, { errorInfo: { code: "upload_payload_too_large" } })).toContain("large");
  });

  it("translates generic invalid request errors used by control APIs", () => {
    expect(apiErrorMessage(ko, { errorInfo: { code: "invalid_request" } }).length).toBeGreaterThan(0);
    expect(apiErrorMessage(en, { errorInfo: { code: "invalid_request" } })).toContain("request");
  });

  it("translates device configuration errors", () => {
    expect(apiErrorMessage(ko, { errorInfo: { code: "config_not_found" } })).toContain("설정");
    expect(apiErrorMessage(en, { errorInfo: { code: "config_not_found" } })).toContain("configuration");
    expect(apiErrorMessage(ko, { errorInfo: { code: "config_write_failed" } })).toContain("저장");
    expect(apiErrorMessage(en, { errorInfo: { code: "config_write_failed" } })).toContain("save");
  });

  it("translates system API and reset errors", () => {
    expect(apiErrorMessage(ko, { errorInfo: { code: "api_key_unsupported" } })).toContain("API 키");
    expect(apiErrorMessage(en, { errorInfo: { code: "api_key_unsupported" } })).toContain("API key");
    expect(apiErrorMessage(ko, { errorInfo: { code: "nothing_selected" } })).toContain("선택");
    expect(apiErrorMessage(en, { errorInfo: { code: "nothing_selected" } })).toContain("Select");
    expect(apiErrorMessage(ko, { errorInfo: { code: "connections_reset_failed" } })).toContain("초기화");
    expect(apiErrorMessage(en, { errorInfo: { code: "connections_reset_failed" } })).toContain("reset");
    expect(apiErrorMessage(ko, { errorInfo: { code: "stats_reset_failed" } })).toContain("통계");
    expect(apiErrorMessage(en, { errorInfo: { code: "stats_reset_failed" } })).toContain("statistics");
  });

  it("falls back when the error code is unknown", () => {
    const error = new Error("raw device error");
    expect(apiErrorMessage(en, error, "Failed.")).toBe("Failed. raw device error");
  });

  it("normalizes local non-API errors", () => {
    expect(fallbackErrorMessage(new Error("local error"))).toBe("local error");
    expect(fallbackErrorMessage("plain error")).toBe("plain error");
    expect(fallbackErrorMessage(404)).toBe("404");
  });

  it("translates known status codes and preserves fallback for unknown codes", () => {
    expect(apiStatusMessage(en, "setup_wifi_connected")).toBe("Wi-Fi is connected.");
    expect(apiStatusMessage(ko, "unknown_status", "fallback status")).toBe("fallback status");
  });
});

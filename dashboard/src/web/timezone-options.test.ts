import { describe, expect, it } from "vitest";
import controlHandlerSource from "../../../components/radar_api_server/control_handler.cpp?raw";
import firmwareHeader from "../../../components/radar_api_server/timezone_catalog.h?raw";
import selectConfig from "../../../packages/namo/selects.yaml?raw";
import { isTimezoneId, normalizeSupportedTimezone, TIMEZONE_IDS } from "./timezone-options";

describe("timezone options", () => {
  it("keeps every configured timezone unique and recognizes supported ids", () => {
    expect(new Set(TIMEZONE_IDS).size).toBe(TIMEZONE_IDS.length);
    expect(TIMEZONE_IDS.every((timezone) => isTimezoneId(timezone))).toBe(true);
  });

  it("normalizes common browser aliases", () => {
    expect(normalizeSupportedTimezone("Etc/UTC")).toBe("UTC");
    expect(normalizeSupportedTimezone("US/Pacific")).toBe("America/Los_Angeles");
    expect(normalizeSupportedTimezone("Asia/Calcutta")).toBe("Asia/Kolkata");
  });

  it("rejects unknown timezone ids instead of guessing", () => {
    expect(normalizeSupportedTimezone("Moon/Sea_of_Tranquility")).toBeNull();
    expect(normalizeSupportedTimezone(undefined)).toBeNull();
  });

  it("matches the firmware catalog and restored select options", () => {
    const firmwareIds = [...firmwareHeader.matchAll(/\{"([^"]+)",\s*"[^"]+"\}/g)].map((match) => match[1]);
    const timezoneSelectBlock = selectConfig.match(/id: device_timezone([\s\S]*?)(?=\n\s*- platform: ld2450)/)?.[1] ?? "";
    const selectIds = [...timezoneSelectBlock.matchAll(/- "([^"]+)"/g)].map((match) => match[1]);

    expect(firmwareIds).toEqual([...TIMEZONE_IDS]);
    expect(selectIds).toEqual([...TIMEZONE_IDS]);
  });

  it("uses an IDF-supported success status for deferred timezone changes", () => {
    expect(controlHandlerSource).toContain(
      'send_json(request, 200, R"({"ok":true,"changed":true,"todayStatsReset":true})")'
    );
    expect(controlHandlerSource).not.toContain(
      'send_json(request, 202, R"({"ok":true,"changed":true,"todayStatsReset":true})")'
    );
  });
});

export function syncEmbeddedFirmwareVersion(source, firmwareVersion) {
  const version = String(firmwareVersion ?? "").trim();
  if (!version) throw new Error("firmware version is empty");

  const pattern = /inline constexpr const char \*FIRMWARE_VERSION = "[^"]*";/;
  if (!pattern.test(source)) throw new Error("embedded firmware version marker is missing");
  return source.replace(
    pattern,
    `inline constexpr const char *FIRMWARE_VERSION = "${version}";`
  );
}

export function embeddedDashboardVersion(source) {
  const match = /inline constexpr const char \*DASHBOARD_VERSION = "([^"]*)";/.exec(source);
  if (!match) throw new Error("embedded dashboard version marker is missing");
  return match[1];
}

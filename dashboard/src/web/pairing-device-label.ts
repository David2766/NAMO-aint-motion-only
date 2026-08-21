export function pairingDeviceShortName(hostname: string | null | undefined, fallback: string): string {
  const value = hostname?.trim() ?? "";
  if (!value) return fallback;

  const shortName = value.replace(/^presence-sensor-?/i, "").trim();
  return shortName || fallback;
}

export function pairingDeviceAddressLabel(address: string): string {
  const value = address.trim();
  if (!value) return "";

  try {
    const normalized = /^https?:\/\//i.test(value) ? value : `http://${value}`;
    return new URL(normalized).hostname;
  } catch {
    return value;
  }
}

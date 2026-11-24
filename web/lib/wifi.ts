export type WifiNetwork = { ssid: string; rssi: number; secured: boolean };
export type WifiInput = { kind: "scan" } | { kind: "connect"; ssid: string; password: string };
export type WifiJob = {
  id: string;
  kind: WifiInput["kind"];
} & (
  | { status: "pending" | "saved" | "failed" | "expired" }
  | { status: "scanned"; networks: WifiNetwork[] }
);
export type WifiState = { ok: true; online: boolean; job: WifiJob | null };

export const WIFI_JOB_TTL_MS = 90_000;
const UTF8 = new TextEncoder();

function utf8Length(value: string): number {
  // TextEncoder silently replaces lone UTF-16 surrogates; reject instead.
  if (value.includes("\0") || /\p{Surrogate}/u.test(value)) return -1;
  return UTF8.encode(value).length;
}

export function isValidWifiSsid(ssid: string): boolean {
  const length = utf8Length(ssid);
  return length >= 1 && length <= 32;
}

export function isValidWifiPassword(password: string): boolean {
  const length = utf8Length(password);
  return length === 0 || (length >= 8 && length <= 63);
}

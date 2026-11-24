import { createCipheriv, randomBytes } from "node:crypto";
import { adminReply, hasSameOrigin, requireAdmin } from "@/lib/auth-server";
import { commandJson } from "@/lib/commands";
import { isValidWifiPassword, isValidWifiSsid, type WifiInput, type WifiNetwork, type WifiState } from "@/lib/wifi";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

const ENDPOINT = "https://proyecto16.cruchi.com/invernadero/api/admin-wifi.php";
const ID = /^[0-9a-f]{32}$/;
const SECRET = /^[0-9a-f]{64}$/;

type BridgeInput = { id: string; kind: "scan" } | { id: string; kind: "connect"; encrypted: string };

function record(value: unknown): Record<string, unknown> | null {
  return typeof value === "object" && value !== null && !Array.isArray(value) ? value as Record<string, unknown> : null;
}

function parseInput(value: unknown): WifiInput | null {
  const input = record(value);
  if (!input) return null;
  const keys = Object.keys(input).length;
  if (input.kind === "scan" && keys === 1) return { kind: "scan" };
  if (input.kind === "connect" && keys === 3 && typeof input.ssid === "string" && typeof input.password === "string" &&
      isValidWifiSsid(input.ssid) && isValidWifiPassword(input.password)) {
    return { kind: "connect", ssid: input.ssid, password: input.password };
  }
  return null;
}

function parseState(value: unknown): WifiState | null {
  const state = record(value);
  if (!state || state.ok !== true || typeof state.online !== "boolean") return null;
  if (state.job === null) return { ok: true, online: state.online, job: null };
  const job = record(state.job);
  if (!job || typeof job.id !== "string" || job.id.length !== 32 || !ID.test(job.id) ||
      (job.kind !== "scan" && job.kind !== "connect")) return null;
  const { id, kind, status } = job;
  if (status === "pending" || status === "expired" || status === "failed" || (status === "saved" && kind === "connect")) {
    return { ok: true, online: state.online, job: { id, kind, status } };
  }
  if (status !== "scanned" || kind !== "scan" || !Array.isArray(job.networks) || job.networks.length > 12) return null;
  const networks: WifiNetwork[] = [];
  for (const value of job.networks) {
    const network = record(value);
    if (!network || typeof network.ssid !== "string" || !isValidWifiSsid(network.ssid) ||
        typeof network.rssi !== "number" || !Number.isInteger(network.rssi) || typeof network.secured !== "boolean") return null;
    networks.push({ ssid: network.ssid, rssi: network.rssi, secured: network.secured });
  }
  return { ok: true, online: state.online, job: { id, kind, status, networks } };
}

function prepare(input: WifiInput, key: string): BridgeInput {
  const id = randomBytes(16).toString("hex");
  if (input.kind === "scan") return { id, kind: "scan" };
  const ssidLength = Buffer.byteLength(input.ssid, "utf8");
  const plaintext = Buffer.alloc(1 + ssidLength + Buffer.byteLength(input.password, "utf8"));
  const nonce = randomBytes(12);
  const cipher = createCipheriv("aes-256-gcm", Buffer.from(key, "hex"), nonce, { authTagLength: 16 });
  cipher.setAAD(Buffer.from(id, "ascii"));
  try {
    plaintext[0] = ssidLength;
    plaintext.write(input.ssid, 1, ssidLength, "utf8");
    plaintext.write(input.password, 1 + ssidLength, "utf8");
    const ciphertext = Buffer.concat([cipher.update(plaintext), cipher.final()]);
    return { id, kind: "connect", encrypted: Buffer.concat([nonce, cipher.getAuthTag(), ciphertext]).toString("hex") };
  } finally {
    plaintext.fill(0);
  }
}

async function bridge(input?: WifiInput) {
  const token = process.env.CRUCHI_WIFI_ADMIN_TOKEN;
  const key = process.env.CRUCHI_WIFI_DEVICE_KEY;
  if (token?.length !== 64 || key?.length !== 64 || !SECRET.test(token) || !SECRET.test(key) || key === token) {
    return adminReply(503, { error: "Administración Wi-Fi sin configurar." });
  }
  try {
    const payload = input ? prepare(input, key) : undefined;
    const response = await fetch(ENDPOINT, {
      method: payload ? "POST" : "GET",
      headers: { Authorization: `Bearer ${token}`, ...(payload ? { "Content-Type": "application/json" } : {}) },
      body: payload ? JSON.stringify(payload) : undefined,
      cache: "no-store",
      redirect: "error",
      signal: AbortSignal.timeout(5_000),
    });
    if (payload && response.status === 409) {
      return adminReply(409, { error: "El ESP32 no tiene conexión reciente o ya hay una operación Wi-Fi pendiente." });
    }
    if (response.ok) {
      const state = parseState(await response.json());
      if (state && (!payload || (state.job?.id === payload.id && state.job.kind === payload.kind))) {
        return adminReply(200, state);
      }
    }
  } catch {
    // Do not log bridge failures: requests carry a private token and encrypted credentials.
  }
  return adminReply(502, { error: "No se pudo confirmar la respuesta del servicio Wi-Fi." });
}

export async function GET() {
  const denied = await requireAdmin();
  if (denied) return denied;
  return bridge();
}

export async function POST(request: Request) {
  const denied = await requireAdmin();
  if (denied) return denied;
  if (!hasSameOrigin(request)) return adminReply(403, { error: "Origen de la solicitud no permitido." });
  if (!/^application\/json(?:\s*;|$)/i.test(request.headers.get("content-type") ?? "")) {
    return adminReply(400, { error: "Se requiere un cuerpo application/json." });
  }
  let raw: unknown;
  try {
    raw = await commandJson(request);
  } catch {
    return adminReply(400, { error: "Solicitud Wi-Fi inválida o demasiado grande." });
  }
  const input = parseInput(raw);
  if (!input) return adminReply(400, { error: "Solicitud Wi-Fi inválida. Revisa el nombre de red y la contraseña." });
  return bridge(input);
}

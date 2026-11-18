import { createHash, timingSafeEqual } from "node:crypto";
import { telemetryDatabase } from "@/lib/supabase-server";
import { validateTelemetry } from "@/lib/telemetry";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";
const MAX_BODY_BYTES = 4_096;

function reply(status: number, body: Record<string, unknown>) {
  return Response.json(body, { status, headers: { "Cache-Control": "no-store" } });
}

async function boundedJson(request: Request): Promise<unknown> {
  const length = request.headers.get("content-length");
  if (length && (!/^\d+$/.test(length) || Number(length) > MAX_BODY_BYTES)) {
    throw new RangeError("body too large");
  }
  if (!request.body) throw new SyntaxError("missing body");
  const reader = request.body.getReader();
  const buffer = new Uint8Array(MAX_BODY_BYTES);
  let size = 0;
  try {
    while (true) {
      const chunk = await reader.read();
      if (chunk.done) break;
      if (size + chunk.value.byteLength > MAX_BODY_BYTES) {
        await reader.cancel();
        throw new RangeError("body too large");
      }
      buffer.set(chunk.value, size);
      size += chunk.value.byteLength;
    }
  } finally {
    reader.releaseLock();
  }
  return JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(buffer.subarray(0, size)));
}

export async function POST(request: Request) {
  const expectedHash = process.env.DEVICE_TOKEN_SHA256;
  const expectedDevice = process.env.DEVICE_ID;
  if (!expectedHash || !/^[0-9a-f]{64}$/.test(expectedHash) || !expectedDevice ||
      !process.env.NEXT_PUBLIC_SUPABASE_URL || !process.env.SUPABASE_SERVICE_ROLE_KEY) {
    return reply(503, { ok: false, error: "Servicio de telemetría sin configurar." });
  }
  const authorization = request.headers.get("authorization") ?? "";
  if (!/^Bearer [0-9a-f]{64}$/.test(authorization)) return reply(401, { ok: false, error: "No autorizado." });
  const receivedHash = createHash("sha256").update(authorization.slice(7)).digest();
  if (!timingSafeEqual(receivedHash, Buffer.from(expectedHash, "hex"))) {
    return reply(401, { ok: false, error: "No autorizado." });
  }
  if (!/^application\/json(?:\s*;|$)/i.test(request.headers.get("content-type") ?? "")) {
    return reply(415, { ok: false, error: "Se requiere application/json." });
  }
  let raw: unknown;
  try {
    raw = await boundedJson(request);
  } catch (error) {
    return reply(error instanceof RangeError ? 413 : 400, { ok: false, error: "Cuerpo de telemetría inválido." });
  }
  const sample = validateTelemetry(raw, expectedDevice);
  if (!sample) return reply(400, { ok: false, error: "Muestra inválida o fuera de los límites de fase 4." });
  try {
    const { data, error } = await telemetryDatabase().rpc("ingest_telemetry_v4", { payload: sample });
    if (error || !data || typeof data.accepted !== "boolean") {
      console.error("Telemetry database rejected request", { code: error?.code });
      return reply(503, { ok: false, error: "No se pudo guardar la muestra." });
    }
    return reply(200, { ok: true, accepted: data.accepted, command: data.accepted ? data.command ?? null : null });
  } catch {
    return reply(503, { ok: false, error: "Servicio de datos no disponible." });
  }
}

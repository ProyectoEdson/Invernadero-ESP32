import "server-only";
import type { CommandInput } from "./types";

const MAX_COMMAND_BYTES = 1_024;

export async function commandJson(request: Request): Promise<unknown> {
  const length = request.headers.get("content-length");
  if (length && (!/^\d+$/.test(length) || Number(length) > MAX_COMMAND_BYTES)) {
    throw new RangeError("body too large");
  }
  if (!request.body) throw new SyntaxError("missing body");
  const reader = request.body.getReader();
  const buffer = new Uint8Array(MAX_COMMAND_BYTES);
  let size = 0;
  try {
    while (true) {
      const chunk = await reader.read();
      if (chunk.done) break;
      if (size + chunk.value.byteLength > MAX_COMMAND_BYTES) {
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

export function validateCommand(value: unknown): CommandInput | null {
  if (typeof value !== "object" || value === null || Array.isArray(value)) return null;
  const input = value as Record<string, unknown>;
  if (Object.keys(input).length !== 2) return null;
  if (input.kind === "set_mode" && (input.value === "manual" || input.value === "automatic")) {
    return { kind: "set_mode", value: input.value };
  }
  if (input.kind === "set_fans" && typeof input.value === "boolean") {
    return { kind: "set_fans", value: input.value };
  }
  if (input.kind === "pump_pulse" && typeof input.duration_ms === "number" &&
      Number.isInteger(input.duration_ms) && input.duration_ms >= 1 && input.duration_ms <= 3_000) {
    return { kind: "pump_pulse", duration_ms: input.duration_ms };
  }
  return null;
}

export const COMMAND_CONFLICTS: Record<string, string> = {
  device_not_ready: "El dispositivo no está listo o no ha enviado una muestra reciente de fase 4.",
  pump_requires_manual: "La bomba solo admite pulsos en modo manual.",
  command_pending: "Ya existe una orden pendiente. Espera su confirmación o vencimiento.",
};

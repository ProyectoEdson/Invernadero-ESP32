import { adminReply, hasSameOrigin, requireAdmin } from "@/lib/auth-server";
import { commandJson, COMMAND_CONFLICTS, validateCommand } from "@/lib/commands";
import { telemetryDatabase } from "@/lib/supabase-server";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function POST(request: Request) {
  const denied = await requireAdmin();
  if (denied) return denied;
  if (!hasSameOrigin(request)) {
    return adminReply(403, { error: "Origen de la solicitud no permitido." });
  }
  if (!/^application\/json(?:\s*;|$)/i.test(request.headers.get("content-type") ?? "")) {
    return adminReply(400, { error: "Se requiere un cuerpo application/json." });
  }
  let raw: unknown;
  try {
    raw = await commandJson(request);
  } catch {
    return adminReply(400, { error: "Cuerpo de la orden inválido o demasiado grande." });
  }
  const command = validateCommand(raw);
  if (!command) return adminReply(400, { error: "Orden inválida. Peltier permanece bloqueada." });
  const deviceId = process.env.DEVICE_ID;
  if (!deviceId || !/^inv-[0-9a-f]{12}$/.test(deviceId)) {
    return adminReply(503, { error: "Dispositivo administrativo sin configurar." });
  }
  try {
    const { data, error } = await telemetryDatabase().rpc("create_device_command", {
      p_device_id: deviceId,
      payload: command,
    });
    if (error) {
      if (Object.hasOwn(COMMAND_CONFLICTS, error.message)) {
        return adminReply(409, { error: COMMAND_CONFLICTS[error.message] });
      }
      if (error.code === "22023") return adminReply(400, { error: "Orden inválida." });
      console.error("Administrative command rejected", { code: error.code });
      return adminReply(503, { error: "No se pudo guardar la orden." });
    }
    if (!data) return adminReply(503, { error: "No se pudo guardar la orden." });
    return adminReply(201, { command: data });
  } catch {
    return adminReply(503, { error: "Servicio de datos no disponible." });
  }
}

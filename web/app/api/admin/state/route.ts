import { adminReply, requireAdmin } from "@/lib/auth-server";
import { telemetryDatabase } from "@/lib/supabase-server";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function GET() {
  const denied = await requireAdmin();
  if (denied) return denied;
  const deviceId = process.env.DEVICE_ID;
  if (!deviceId || !/^inv-[0-9a-f]{12}$/.test(deviceId)) {
    return adminReply(503, { error: "Dispositivo administrativo sin configurar." });
  }
  try {
    const { data, error } = await telemetryDatabase().rpc("admin_device_state", { p_device_id: deviceId });
    if (error || !data) {
      console.error("Administrative state unavailable", { code: error?.code });
      return adminReply(503, { error: "No se pudo consultar el estado administrativo." });
    }
    return adminReply(200, data);
  } catch {
    return adminReply(503, { error: "Servicio de datos no disponible." });
  }
}

import "server-only";
import { createServerClient } from "@supabase/ssr";
import { cookies, headers } from "next/headers";

/** Crear por petición; los Route Handlers propagan también la renovación de sesión. */
export async function authClient() {
  const url = process.env.NEXT_PUBLIC_SUPABASE_URL;
  const key = process.env.NEXT_PUBLIC_SUPABASE_ANON_KEY;
  if (!url || !key) throw new Error("Supabase authentication configuration is missing");
  const cookieStore = await cookies();
  const requestHeaders = await headers();
  return createServerClient(url, key, {
    cookieOptions: {
      sameSite: "lax",
      path: "/",
      secure: process.env.NODE_ENV === "production" || requestHeaders.get("x-forwarded-proto") === "https",
    },
    cookies: {
      getAll: () => cookieStore.getAll(),
      setAll: (values) => {
        for (const { name, value, options } of values) cookieStore.set(name, value, options);
      },
    },
  });
}

export function adminReply(status: number, body: Record<string, unknown>) {
  return Response.json(body, {
    status,
    headers: { "Cache-Control": "private, no-store", Vary: "Cookie" },
  });
}

export async function requireAdmin(): Promise<Response | null> {
  const adminId = process.env.ADMIN_USER_ID;
  if (!adminId || !/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(adminId)) {
    return adminReply(503, { error: "Acceso administrativo sin configurar." });
  }
  try {
    const client = await authClient();
    const { data: { user }, error } = await client.auth.getUser();
    if (error || !user) return adminReply(401, { error: "Inicia sesión para continuar." });
    if (user.id.toLowerCase() !== adminId.toLowerCase()) {
      return adminReply(403, { error: "Esta cuenta no tiene acceso administrativo." });
    }
    return null;
  } catch {
    return adminReply(503, { error: "Servicio de autenticación no disponible." });
  }
}

export function hasSameOrigin(request: Request): boolean {
  const origin = request.headers.get("origin");
  if (!origin) return false;
  try {
    return new URL(origin).origin === origin && origin === new URL(request.url).origin;
  } catch {
    return false;
  }
}

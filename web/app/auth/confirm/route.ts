import { NextResponse, type NextRequest } from "next/server";
import { authClient } from "../../../lib/auth-server";
import { authDestination } from "../destination";

export async function GET(request: NextRequest) {
  const tokenHash = request.nextUrl.searchParams.get("token_hash");
  const type = request.nextUrl.searchParams.get("type");
  if (tokenHash && (type === "invite" || type === "recovery")) {
    try {
      const client = await authClient();
      const { error } = await client.auth.verifyOtp({ token_hash: tokenHash, type });
      if (!error) return NextResponse.redirect(new URL(authDestination(request.nextUrl.searchParams.get("next")), request.url));
    } catch {
      // Mantener tokens y errores internos fuera de la respuesta pública.
    }
  }
  return NextResponse.redirect(new URL("/auth/password?error=link", request.url));
}

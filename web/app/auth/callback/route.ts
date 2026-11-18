import { NextResponse, type NextRequest } from "next/server";
import { authClient } from "../../../lib/auth-server";
import { authDestination } from "../destination";

export async function GET(request: NextRequest) {
  const code = request.nextUrl.searchParams.get("code");
  if (code) {
    try {
      const client = await authClient();
      const { error } = await client.auth.exchangeCodeForSession(code);
      if (!error) return NextResponse.redirect(new URL(authDestination(request.nextUrl.searchParams.get("next")), request.url));
    } catch {
      // No incluir códigos, tokens ni detalles de Supabase en la URL de error.
    }
  }
  return NextResponse.redirect(new URL("/auth/password?error=link", request.url));
}

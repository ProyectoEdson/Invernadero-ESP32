/** Solo destinos privados conocidos; nunca redirigir a un origen recibido por URL. */
export function authDestination(next: string | null): string {
  return next === "/admin" ? "/admin" : "/auth/password";
}

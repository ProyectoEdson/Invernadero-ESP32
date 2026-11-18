"use client";

import { useEffect, useState, type FormEvent } from "react";
import { browserAuth } from "../lib/supabase-browser";
import { AdminShell } from "./admin-shell";

type Access = "checking" | "ready" | "denied" | "invalid" | "error" | "saved";

export function AdminPassword() {
  const [access, setAccess] = useState<Access>("checking");
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  useEffect(() => {
    const controller = new AbortController();
    let active = true;
    async function check() {
      if (new URLSearchParams(window.location.search).has("error")) {
        setAccess("invalid");
        return;
      }
      try {
        const { data, error: sessionError } = await browserAuth().auth.getUser();
        if (!active) return;
        if (sessionError || !data.user) { setAccess("invalid"); return; }
        const response = await fetch("/api/admin/state", { cache: "no-store", credentials: "same-origin", signal: controller.signal });
        if (!active) return;
        setAccess(response.ok ? "ready" : response.status === 403 ? "denied" : response.status === 401 ? "invalid" : "error");
      } catch {
        if (active) setAccess("error");
      }
    }
    void check();
    return () => { active = false; controller.abort(); };
  }, []);

  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (saving || access !== "ready") return;
    const fields = new FormData(event.currentTarget);
    const password = String(fields.get("password") ?? "");
    if (password.length < 12) { setError("Usa al menos 12 caracteres para tu contraseña."); return; }
    if (password !== String(fields.get("confirmation") ?? "")) { setError("Las contraseñas no coinciden."); return; }
    setSaving(true);
    setError(null);
    try {
      const { error: updateError } = await browserAuth().auth.updateUser({ password });
      if (updateError) {
        setError(updateError.code === "same_password" ? "Elige una contraseña distinta de la actual." : "No se pudo guardar la contraseña. Comprueba los requisitos o solicita un nuevo enlace al administrador.");
      } else {
        setAccess("saved");
      }
    } catch {
      setError("No se pudo conectar. Comprueba la conexión antes de volver a guardar.");
    } finally {
      setSaving(false);
    }
  }

  return (
    <AdminShell>
      <section className="admin-auth" aria-labelledby="password-title">
        <div className="admin-auth-context"><h1 id="password-title">Tu acceso al invernadero</h1><p>Establece una contraseña para la cuenta autorizada. Este acceso no permite registrar usuarios.</p><a className="admin-inline-link" href="/admin">Volver a administración</a></div>
        <div className="admin-auth-form">
          {access === "checking" && <p role="status">Comprobando el enlace y los permisos…</p>}
          {access === "invalid" && <div className="notice" role="alert"><strong>El enlace no es válido o ha caducado.</strong><p>Abre el último enlace de invitación o recuperación desde tu correo. Si ya lo usaste, entra con tu contraseña o solicita otro al administrador.</p></div>}
          {access === "denied" && <div className="notice" role="alert"><strong>Cuenta sin permisos de administración.</strong><p>Este enlace no corresponde al propietario autorizado. Cierra la sesión desde administración antes de usar otra cuenta.</p></div>}
          {access === "error" && <div className="notice" role="alert"><strong>No se pudo verificar el acceso.</strong><p>Comprueba la conexión y vuelve a abrir esta página. No se ha cambiado la contraseña.</p></div>}
          {access === "saved" && <div role="status"><h2>Contraseña guardada</h2><p>Ya puedes abrir el panel privado con tu cuenta autorizada.</p><a className="admin-button" href="/admin">Abrir administración</a></div>}
          {access === "ready" && <form onSubmit={submit}>
            <h2>Establecer contraseña</h2>
            <label htmlFor="password">Nueva contraseña</label><input id="password" name="password" type="password" autoComplete="new-password" minLength={12} required disabled={saving} aria-describedby="password-help" />
            <p id="password-help" className="admin-help">Al menos 12 caracteres. No compartas tu contraseña.</p>
            <label htmlFor="confirmation">Repite la contraseña</label><input id="confirmation" name="confirmation" type="password" autoComplete="new-password" minLength={12} required disabled={saving} />
            {error && <p className="admin-error" role="alert">{error}</p>}
            <button className="admin-button" type="submit" disabled={saving}>{saving ? "Guardando…" : "Guardar contraseña"}</button>
          </form>}
        </div>
      </section>
    </AdminShell>
  );
}

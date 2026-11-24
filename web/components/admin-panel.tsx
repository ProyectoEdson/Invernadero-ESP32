"use client";

import { useCallback, useEffect, useRef, useState, type FormEvent } from "react";
import { browserAuth } from "../lib/supabase-browser";
import { sampleAgeMs, STALE_AFTER_MS, type Command, type CommandInput, type DeviceState } from "../lib/types";
import { AdminShell } from "./admin-shell";
import { AdminWifi } from "./admin-wifi";

type Access = "checking" | "login" | "allowed" | "denied";
type AdminState = { state: DeviceState | null; commands: Command[] };
const statusLabels = { pending: "Pendiente", applied: "Aplicada", rejected: "Rechazada", expired: "Caducada" };

function commandLabel(command: Command | CommandInput) {
  if (command.kind === "set_mode") return command.value === "automatic" ? "Cambiar a automático" : "Cambiar a manual";
  if (command.kind === "set_fans") return command.value ? "Encender ventiladores" : "Apagar ventiladores";
  return `Pulso de bomba de ${command.duration_ms} ms`;
}

function dateLabel(value: string) {
  return new Intl.DateTimeFormat("es", { day: "2-digit", month: "short", hour: "2-digit", minute: "2-digit", second: "2-digit" }).format(new Date(value));
}

export function AdminPanel() {
  const [access, setAccess] = useState<Access>("checking");
  const [data, setData] = useState<AdminState>({ state: null, commands: [] });
  const [now, setNow] = useState<number | null>(null);
  const [online, setOnline] = useState<boolean | null>(null);
  const [queryError, setQueryError] = useState<string | null>(null);
  const [authMessage, setAuthMessage] = useState<string | null>(null);
  const [authError, setAuthError] = useState<string | null>(null);
  const [commandError, setCommandError] = useState<string | null>(null);
  const [commandMessage, setCommandMessage] = useState<string | null>(null);
  const [authBusy, setAuthBusy] = useState(false);
  const [sending, setSending] = useState(false);
  const [duration, setDuration] = useState("1000");
  const mounted = useRef(false);
  const currentAccess = useRef<Access>("checking");
  const hadAccess = useRef(false);
  const request = useRef<AbortController | null>(null);
  const mutation = useRef<AbortController | null>(null);
  const submitting = useRef(false);

  const changeAccess = useCallback((next: Access) => {
    currentAccess.current = next;
    setAccess(next);
    if (next === "login" || next === "denied") setData({ state: null, commands: [] });
  }, []);

  const refresh = useCallback(async () => {
    if (!mounted.current || request.current || submitting.current) return;
    const controller = new AbortController();
    request.current = controller;
    try {
      const response = await fetch("/api/admin/state", {
        cache: "no-store", credentials: "same-origin",
        signal: AbortSignal.any([controller.signal, AbortSignal.timeout(8_000)]),
      });
      if (!mounted.current || controller.signal.aborted) return;
      if (response.status === 401) {
        if (hadAccess.current) setAuthError("La sesión ha caducado. Vuelve a entrar para enviar órdenes.");
        changeAccess("login");
        return;
      }
      if (response.status === 403) { changeAccess("denied"); return; }
      if (!response.ok) throw new Error("state");
      const result: AdminState = await response.json();
      if (!mounted.current || controller.signal.aborted) return;
      setData({ state: result.state, commands: result.commands.slice(0, 20) });
      setQueryError(null);
      hadAccess.current = true;
      changeAccess("allowed");
    } catch {
      if (mounted.current && !controller.signal.aborted) setQueryError("No se pudo actualizar el panel. Los mandos están bloqueados hasta recuperar la conexión.");
    } finally {
      if (request.current === controller) request.current = null;
    }
  }, [changeAccess]);

  useEffect(() => {
    mounted.current = true;
    setNow(Date.now());
    setOnline(navigator.onLine);
    void refresh();
    const clock = window.setInterval(() => setNow(Date.now()), 1_000);
    const poll = window.setInterval(() => {
      if (currentAccess.current === "allowed" || currentAccess.current === "checking") void refresh();
    }, 3_000);
    const connected = () => { setOnline(true); if (currentAccess.current === "allowed" || currentAccess.current === "checking") void refresh(); };
    const disconnected = () => setOnline(false);
    window.addEventListener("online", connected);
    window.addEventListener("offline", disconnected);
    return () => {
      mounted.current = false;
      window.clearInterval(clock);
      window.clearInterval(poll);
      request.current?.abort();
      mutation.current?.abort();
      window.removeEventListener("online", connected);
      window.removeEventListener("offline", disconnected);
    };
  }, [refresh]);

  async function login(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (authBusy) return;
    const fields = new FormData(event.currentTarget);
    setAuthBusy(true);
    setAuthError(null);
    setAuthMessage(null);
    try {
      const { error } = await browserAuth().auth.signInWithPassword({ email: String(fields.get("email") ?? "").trim(), password: String(fields.get("password") ?? "") });
      if (error) {
        setAuthError("No se pudo iniciar sesión. Revisa el correo y la contraseña de la cuenta autorizada.");
        return;
      }
      await refresh();
    } catch {
      setAuthError("No se pudo conectar al acceso privado. Comprueba la conexión y la configuración del despliegue.");
    } finally {
      if (mounted.current) setAuthBusy(false);
    }
  }

  async function recover(form: HTMLFormElement | null) {
    if (!form || authBusy) return;
    const emailInput = form.elements.namedItem("email") as HTMLInputElement;
    if (!emailInput.reportValidity()) return;
    setAuthBusy(true);
    setAuthError(null);
    setAuthMessage(null);
    try {
      const { error } = await browserAuth().auth.resetPasswordForEmail(emailInput.value.trim(), { redirectTo: `${window.location.origin}/auth/confirm` });
      if (error) setAuthError("No se pudo solicitar el enlace. Espera un momento y vuelve a intentarlo.");
      else setAuthMessage("Si el correo tiene una cuenta habilitada, recibirás un enlace para establecer una contraseña. Revisa también la carpeta de correo no deseado.");
    } catch {
      setAuthError("No se pudo conectar para solicitar el enlace de recuperación.");
    } finally {
      if (mounted.current) setAuthBusy(false);
    }
  }

  async function logout() {
    if (authBusy || submitting.current) return;
    submitting.current = true;
    setAuthBusy(true);
    setAuthError(null);
    request.current?.abort();
    try {
      const { error } = await browserAuth().auth.signOut();
      if (error) { setAuthError("No se pudo cerrar la sesión. Vuelve a intentarlo."); return; }
      hadAccess.current = false;
      setQueryError(null);
      setCommandMessage(null);
      setCommandError(null);
      changeAccess("login");
    } catch {
      setAuthError("No se pudo cerrar la sesión. Comprueba la conexión y vuelve a intentarlo.");
    } finally {
      submitting.current = false;
      if (mounted.current) setAuthBusy(false);
    }
  }

  const state = data.state;
  const age = state && now !== null ? sampleAgeMs(state, now) : null;
  const fresh = age !== null && Number.isFinite(age) && age < STALE_AFTER_MS;
  const pending = data.commands.find((command) => command.status === "pending");
  const controlReady = access === "allowed" && online === true && !queryError && fresh && state?.wifi_connected === true && state.phase4_control_ready === true && (state.operating_mode === "manual" || state.operating_mode === "automatic");
  const disabled = !controlReady || Boolean(pending) || sending || authBusy;
  const modeLabel = state?.operating_mode === "manual" ? "Manual" : state?.operating_mode === "automatic" ? "Automático" : state?.operating_mode === "monitoring" ? "Monitorización" : "Sin confirmar";
  const unavailable = online === false ? "Sin conexión a internet. Mandos deshabilitados." : queryError ? queryError : !state ? "Aún no hay una lectura del dispositivo." : !fresh ? "Dispositivo sin lectura reciente (menos de 15 s). Mandos deshabilitados." : state.phase4_control_ready !== true ? "El firmware no ha confirmado que el control de fase 4 esté listo." : state.wifi_connected !== true ? "El dispositivo no confirma conexión Wi-Fi." : state.operating_mode === "monitoring" ? "El dispositivo solo ha confirmado monitorización." : pending ? "Hay una orden pendiente. Espera la respuesta del dispositivo antes de enviar otra." : null;

  async function send(input: CommandInput) {
    if (disabled || submitting.current) return;
    if (input.kind === "pump_pulse" && state?.operating_mode !== "manual") return;
    const message = input.kind === "set_mode"
      ? input.value === "automatic" ? "¿Cambiar a automático? En fase 4 no hay riego automático; la calibración y la programación quedan para fase 5. El cambio apaga la bomba." : "¿Cambiar a manual? El cambio de modo apaga la bomba."
      : input.kind === "pump_pulse" ? `¿Solicitar un pulso de ${input.duration_ms} ms en GPIO5? Mantén el montaje sin cargas.`
      : `¿${input.value ? "Encender" : "Apagar"} ventiladores (GPIO7)? El estado solo cambiará aquí cuando lo confirme el dispositivo.`;
    if (!window.confirm(message)) return;
    submitting.current = true;
    setSending(true);
    setCommandError(null);
    setCommandMessage(null);
    request.current?.abort();
    const controller = new AbortController();
    mutation.current = controller;
    try {
      const response = await fetch("/api/admin/commands", {
        method: "POST", credentials: "same-origin", headers: { "Content-Type": "application/json" }, body: JSON.stringify(input),
        signal: AbortSignal.any([controller.signal, AbortSignal.timeout(10_000)]),
      });
      if (!mounted.current || controller.signal.aborted) return;
      if (response.status === 401) { setAuthError("La sesión ha caducado. Vuelve a entrar; no se ha confirmado ninguna actuación."); changeAccess("login"); return; }
      if (response.status === 403) { changeAccess("denied"); return; }
      if (!response.ok) {
        setQueryError("Esperando una nueva lectura del panel antes de habilitar los mandos.");
        setCommandError(response.status === 409 ? "Orden no aceptada: el dispositivo no está listo, el modo no lo permite o ya hay otra orden pendiente. Revisa el estado actualizado." : response.status === 400 ? "La orden no es válida. Revisa los parámetros antes de enviarla de nuevo." : "El servidor no pudo confirmar la recepción de la orden. Consulta el historial antes de volver a enviarla.");
        return;
      }
      const result: { command: Command } = await response.json();
      if (!mounted.current || controller.signal.aborted) return;
      setData((previous) => ({ ...previous, commands: [result.command, ...previous.commands.filter((command) => command.id !== result.command.id)].slice(0, 20) }));
      setCommandMessage("Orden recibida por el servidor. Todavía no confirma actuación: espera a que aparezca como aplicada, rechazada o caducada.");
    } catch {
      if (mounted.current && !controller.signal.aborted) {
        setCommandError("No se recibió confirmación del servidor. La orden podría haberse registrado: revisa el historial antes de volver a enviarla.");
        setQueryError("Esperando una nueva lectura del panel antes de habilitar los mandos.");
      }
    } finally {
      if (mutation.current === controller) mutation.current = null;
      submitting.current = false;
      if (mounted.current) { setSending(false); void refresh(); }
    }
  }

  return (
    <AdminShell action={access === "allowed" || access === "denied" ? <button className="admin-quiet-button" type="button" onClick={() => void logout()} disabled={authBusy || sending}>{authBusy ? "Cerrando…" : "Cerrar sesión"}</button> : undefined}>
      {access === "checking" && <section className="admin-loading"><h1>Administración</h1><p role="status">Comprobando sesión y permisos…</p>{queryError && <div className="notice" role="alert">{queryError}<button className="text-button" onClick={() => void refresh()}>Volver a comprobar</button></div>}</section>}
      {access === "login" && <section className="admin-auth" aria-labelledby="login-title">
        <div className="admin-auth-context"><h1 id="login-title">El cuidado, en tus manos.</h1><p>Panel privado del invernadero. Consulta las lecturas, cambia el modo y solicita órdenes con confirmación del ESP32.</p><div className="admin-botanical" aria-hidden="true"><svg viewBox="0 0 240 160"><path d="M120 148V45M120 107C76 112 45 90 45 62C86 58 115 77 120 107ZM120 77C163 80 193 57 193 27C153 25 124 46 120 77ZM120 139C157 142 177 129 183 106C151 100 130 115 120 139M120 107L70 77M120 77L171 44" /></svg></div><p className="admin-help">Acceso solo para la cuenta autorizada. No hay registro público.</p></div>
        <form className="admin-auth-form" onSubmit={login}>
          <h2>Entrar a administración</h2>
          <label htmlFor="admin-email">Correo electrónico</label><input id="admin-email" name="email" type="email" autoComplete="username" required disabled={authBusy} />
          <label htmlFor="admin-password">Contraseña</label><input id="admin-password" name="password" type="password" autoComplete="current-password" required disabled={authBusy} />
          {authError && <p className="admin-error" role="alert">{authError}</p>}{queryError && <p className="admin-error" role="alert">{queryError}</p>}{authMessage && <p className="admin-help" role="status">{authMessage}</p>}
          <button className="admin-button" type="submit" disabled={authBusy}>{authBusy ? "Comprobando…" : "Entrar"}</button>
          <button className="admin-recovery" type="button" disabled={authBusy} onClick={(event) => void recover(event.currentTarget.form)}>Recuperar contraseña</button>
          <p className="admin-help">Primera vez: abre el enlace de invitación recibido por correo para establecer tu contraseña.</p>
        </form>
      </section>}
      {access === "denied" && <section className="admin-loading"><h1>Acceso no autorizado</h1><div className="notice" role="alert"><p>La sesión es válida, pero esta cuenta no tiene permisos de propietario. No se muestran mandos ni órdenes privadas. Cierra la sesión y entra con la cuenta autorizada.</p></div>{authError && <p className="admin-error" role="alert">{authError}</p>}</section>}
      {access === "allowed" && <>
        <div className="admin-intro"><div><h1>Cuida el invernadero</h1><p>Estado reportado y órdenes con confirmación. Actualización cada 3 segundos.</p></div><div className={`admin-availability ${controlReady ? "is-ready" : ""}`}><span className={`status-dot ${controlReady ? "is-live" : ""}`} aria-hidden="true" />{controlReady ? "Dispositivo listo" : "Control no disponible"}</div></div>
        <div className="admin-safety"><strong>Montaje de prueba sin cargas</strong><p>Los niveles GPIO indican lo que reporta el firmware, no confirman actuación física, caudal ni ventilación real. Mantén desconectadas las cargas. Peltier permanece bloqueada.</p></div>
        <div className="admin-feedback" aria-live="polite" aria-atomic="true">{unavailable && <p className="notice">{unavailable}</p>}{authError && <p className="admin-error">{authError}</p>}{commandError && <p className="admin-error">{commandError}</p>}{commandMessage && <p className="admin-help">{commandMessage}</p>}{pending && <p className="admin-pending"><strong>Pendiente:</strong> {commandLabel(pending)}. Aún sin confirmación de aplicación.</p>}</div>
        <section className="admin-live" aria-labelledby="admin-live-title"><div className="admin-section-heading"><h2 id="admin-live-title">Lectura del dispositivo</h2><span>{age !== null && Number.isFinite(age) ? `Muestra de hace ${Math.floor(age / 1_000)} s` : "Sin muestra"}{queryError ? " · Sin actualizar" : ""}</span></div><dl className="admin-live-values"><div><dt>Temperatura</dt><dd>{state?.sensor_status === "ok" && state.temperature_c !== null ? `${state.temperature_c.toFixed(1)} °C` : "Sin lectura válida"}</dd></div><div><dt>Humedad ambiental</dt><dd>{state?.sensor_status === "ok" && state.humidity_pct !== null ? `${state.humidity_pct.toFixed(1)} %` : "Sin lectura válida"}</dd></div><div><dt>Modo reportado</dt><dd>{modeLabel}</dd></div><div><dt>Sensor</dt><dd>{!state ? "Sin confirmar" : state.sensor_status === "ok" ? "Lectura válida" : state.sensor_status === "out_of_range" ? "Fuera de rango" : "Error de lectura"}</dd></div></dl>{state && <p className="admin-help">Última recepción: <time dateTime={state.received_at}>{dateLabel(state.received_at)}</time>. {fresh ? "Lectura reciente." : "Lectura desactualizada: no representa el estado actual."}</p>}</section>
        <section className="admin-controls" aria-labelledby="admin-controls-title"><div className="admin-section-heading"><h2 id="admin-controls-title">Control del montaje</h2><span>Solo el ESP32 confirma la aplicación</span></div>
          <div className="admin-control-row"><div><h3>Modo de operación</h3><p>Reportado: <strong>{modeLabel}</strong>. Cambiar de modo detiene la bomba.</p><p className="admin-help">Automático no riega en fase 4. La calibración y el riego automático quedan para fase 5.</p></div><div className="admin-mode-buttons" role="group" aria-label="Solicitar modo"><button type="button" className="admin-option" disabled={disabled || state?.operating_mode === "manual"} onClick={() => void send({ kind: "set_mode", value: "manual" })}>Solicitar manual</button><button type="button" className="admin-option" disabled={disabled || state?.operating_mode === "automatic"} onClick={() => void send({ kind: "set_mode", value: "automatic" })}>Solicitar automático</button></div></div>
          <div className="admin-control-row"><div><h3>Bomba · GPIO5</h3><p>Reportado: <strong>{!state ? "Sin confirmar" : state.gpio_levels.pump === 1 ? "GPIO alto" : "GPIO bajo"}</strong>. Solo un pulso en modo manual.</p><p className="admin-help">Duración máxima de 3 segundos. Nunca queda encendida por una orden continua.</p></div><form className="admin-pulse" onSubmit={(event) => { event.preventDefault(); const ms = Number(duration); if (Number.isInteger(ms) && ms >= 1 && ms <= 3_000) void send({ kind: "pump_pulse", duration_ms: ms }); }}><label htmlFor="pulse-duration">Duración en milisegundos</label><div><input id="pulse-duration" name="duration_ms" type="number" min={1} max={3_000} step={1} inputMode="numeric" required value={duration} onChange={(event) => setDuration(event.target.value)} disabled={disabled || state?.operating_mode !== "manual"} /><button className="admin-button" disabled={disabled || state?.operating_mode !== "manual"} type="submit">Solicitar pulso</button></div></form></div>
          <div className="admin-control-row"><div><h3>Ventiladores · GPIO7</h3><p>Reportado: <strong>{!state ? "Sin confirmar" : state.gpio_levels.fans === 1 ? "GPIO alto" : "GPIO bajo"}</strong>.</p><p className="admin-help">Permanecen encendidos hasta una orden de apagado o un reinicio. El interruptor muestra solo el estado reportado.</p></div>{state ? <button className="admin-fan-switch" type="button" role="switch" aria-label="Ventiladores, estado reportado por el dispositivo" aria-checked={state.gpio_levels.fans === 1} disabled={disabled} onClick={() => void send({ kind: "set_fans", value: state.gpio_levels.fans !== 1 })}><span className="admin-switch-track" aria-hidden="true"><span /></span><span>{state.gpio_levels.fans === 1 ? "Solicitar apagado" : "Solicitar encendido"}</span></button> : <button className="admin-option" type="button" disabled>Sin estado confirmado</button>}</div>
          <div className="admin-control-row admin-locked"><div><h3>Peltier · GPIO4</h3><p>Sin control disponible. Debe permanecer en nivel bajo.</p></div><span className="admin-lock-state">{state ? state.peltier_locked && state.gpio_levels.peltier === 0 ? "Bloqueo confirmado · GPIO bajo" : "Atención: bloqueo no confirmado" : "Bloqueo sin confirmar"}</span></div>
        </section>
        <AdminWifi />
        <section className="admin-history" aria-labelledby="admin-history-title"><div className="admin-section-heading"><h2 id="admin-history-title">Últimas 20 órdenes</h2><span>Sin cambios optimistas</span></div><p className="admin-help">Pendiente: recibida por el servidor. Aplicada o rechazada: respuesta del ESP32. Caducada: no se confirmó dentro de su validez; no se reenvía como una orden nueva.</p>{data.commands.length === 0 ? <p className="admin-empty">Todavía no hay órdenes registradas. El estado del montaje se muestra arriba.</p> : <div className="admin-orders-scroll" tabIndex={0} role="region" aria-label="Historial de órdenes, desplazamiento horizontal"><table className="admin-orders"><caption>Órdenes más recientes primero. Las razones se conservan tal como las informa el sistema.</caption><thead><tr><th scope="col">Solicitada</th><th scope="col">Orden</th><th scope="col">Estado</th><th scope="col">Motivo / confirmación</th></tr></thead><tbody>{data.commands.map((command) => <tr key={command.id}><td><time dateTime={command.created_at}>{dateLabel(command.created_at)}</time></td><td>{commandLabel(command)}</td><td><span className={`admin-command-status is-${command.status}`}>{statusLabels[command.status]}</span></td><td>{command.reason || (command.status === "pending" ? `Espera confirmación; vence ${dateLabel(command.expires_at)}` : command.status === "applied" ? "Confirmada por el ESP32" : command.status === "expired" ? "Se agotó el plazo de confirmación" : "El dispositivo rechazó la orden")}{command.completed_at && <span className="admin-completed">{dateLabel(command.completed_at)}</span>}</td></tr>)}</tbody></table></div>}</section>
      </>}
    </AdminShell>
  );
}

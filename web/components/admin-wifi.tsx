"use client";

import { useCallback, useEffect, useRef, useState, type FormEvent } from "react";
import { isValidWifiPassword, isValidWifiSsid, WIFI_JOB_TTL_MS, type WifiInput, type WifiJob, type WifiNetwork, type WifiState } from "../lib/wifi";

const ENDPOINT = "/api/admin/wifi";
const POLL_MS = 3_000;
const STATUS_MAX_AGE_MS = 2 * POLL_MS;
const REQUEST_TIMEOUT_MS = 10_000;

function jobMessage(job: WifiJob) {
  switch (job.status) {
    case "pending": return job.kind === "scan" ? "Escaneo pendiente de confirmación del ESP32." : "Cambio pendiente. El ESP32 puede dejar de responder mientras prueba la nueva red.";
    case "scanned": return job.networks.length === 0 ? "Escaneo terminado: no se encontraron redes. Puedes escribir el SSID de una red oculta." : "Escaneo terminado. Selecciona una red o escribe su SSID.";
    case "saved": return "El ESP32 confirmó que probó y guardó la nueva red.";
    case "failed": return job.kind === "scan" ? "El ESP32 no pudo completar el escaneo." : "El ESP32 no pudo guardar la nueva red. Revisa el nombre y la contraseña antes de volver a intentarlo.";
    case "expired": return "La operación caducó sin confirmación. No se reenvió.";
  }
}

export function AdminWifi() {
  const [state, setState] = useState<WifiState | null>(null);
  const [networks, setNetworks] = useState<WifiNetwork[] | null>(null);
  const [ssid, setSsid] = useState("");
  const [sending, setSending] = useState(false);
  const [uncertain, setUncertain] = useState(false);
  const [queryError, setQueryError] = useState<string | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);
  const latest = useRef<{ state: WifiState; at: number } | null>(null);
  const request = useRef<AbortController | null>(null);
  const mutation = useRef<AbortController | null>(null);
  const passwordField = useRef<HTMLInputElement | null>(null);

  const publish = useCallback((next: WifiState | null) => {
    latest.current = next ? { state: next, at: Date.now() } : null;
    setState(next);
    if (next?.job?.status === "scanned") setNetworks(next.job.networks);
  }, []);

  useEffect(() => {
    function disconnected() {
      request.current?.abort();
      publish(null);
      setQueryError("Este navegador no tiene conexión. No se pueden enviar operaciones Wi-Fi.");
    }

    async function refresh() {
      if (!navigator.onLine) {
        disconnected();
        return;
      }
      if (mutation.current) return;
      if (latest.current && Date.now() - latest.current.at >= STATUS_MAX_AGE_MS) publish(null);
      if (request.current) return;
      const controller = new AbortController();
      request.current = controller;
      try {
        const response = await fetch(ENDPOINT, {
          cache: "no-store",
          signal: AbortSignal.any([controller.signal, AbortSignal.timeout(REQUEST_TIMEOUT_MS)]),
        });
        const body = await response.json();
        if (controller.signal.aborted) return;
        if (!response.ok) {
          setQueryError(body.error ?? "No se pudo obtener el estado Wi-Fi.");
          publish(null);
          return;
        }
        publish(body as WifiState);
        setQueryError(null);
      } catch {
        if (controller.signal.aborted) return;
        setQueryError("No se pudo obtener el estado Wi-Fi.");
        publish(null);
      } finally {
        if (request.current === controller) request.current = null;
      }
    }

    void refresh();
    const interval = window.setInterval(() => void refresh(), POLL_MS);
    window.addEventListener("online", refresh);
    window.addEventListener("offline", disconnected);
    return () => {
      window.clearInterval(interval);
      window.removeEventListener("online", refresh);
      window.removeEventListener("offline", disconnected);
      request.current?.abort();
      request.current = null;
      mutation.current?.abort();
      mutation.current = null;
    };
  }, [publish]);

  useEffect(() => {
    if (!uncertain) return;
    const timer = window.setTimeout(() => setUncertain(false), WIFI_JOB_TTL_MS);
    return () => window.clearTimeout(timer);
  }, [uncertain]);

  async function send(input: WifiInput) {
    const snapshot = latest.current;
    if (mutation.current || uncertain || !navigator.onLine || !snapshot?.state.online ||
        snapshot.state.job?.status === "pending" || Date.now() - snapshot.at >= STATUS_MAX_AGE_MS) return;

    const controller = new AbortController();
    mutation.current = controller;
    request.current?.abort();
    setSending(true);
    setActionError(null);
    if (input.kind === "connect" && passwordField.current) passwordField.current.value = "";
    try {
      const response = await fetch(ENDPOINT, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(input),
        cache: "no-store",
        signal: AbortSignal.any([controller.signal, AbortSignal.timeout(REQUEST_TIMEOUT_MS)]),
      });
      const body = await response.json();
      if (controller.signal.aborted) return;
      if (response.ok && body.ok === true) {
        publish(navigator.onLine ? body as WifiState : null);
        if (navigator.onLine) setQueryError(null);
      } else if ([400, 401, 403, 409].includes(response.status)) {
        publish(null);
        setActionError(body.error ?? "No se pudo solicitar la operación Wi-Fi.");
      } else {
        throw new Error("Unconfirmed Wi-Fi operation");
      }
    } catch {
      if (controller.signal.aborted) return;
      publish(null);
      setUncertain(true);
      setActionError("No se confirmó si la solicitud llegó. No se reenviará. Consulta el resultado antes de volver a intentarlo.");
    } finally {
      if (mutation.current === controller) {
        mutation.current = null;
        setSending(false);
      }
    }
  }

  function selectSsid(value: string) {
    if (value !== ssid && passwordField.current) passwordField.current.value = "";
    setSsid(value);
  }

  function connect(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    const password = passwordField.current?.value ?? "";
    if (!isValidWifiSsid(ssid) || !isValidWifiPassword(password)) {
      setActionError("El SSID debe tener de 1 a 32 bytes. La contraseña debe tener de 8 a 63 bytes o quedar vacía si la red es abierta.");
      return;
    }
    if (window.confirm("El ESP32 puede desconectarse mientras prueba la nueva red. ¿Quieres solicitar el cambio de Wi-Fi?")) {
      void send({ kind: "connect", ssid, password });
    }
  }

  const locked = sending || uncertain || state?.job?.status === "pending";
  const disabled = locked || state?.online !== true;
  return (
    <section className="admin-controls admin-wifi" aria-labelledby="admin-wifi-title">
      <div className="admin-section-heading">
        <h2 id="admin-wifi-title">Wi-Fi del ESP32</h2>
        <span role="status">{state?.online === true ? "En línea · señal reciente del ESP32" : state?.online === false ? "Sin señal reciente del ESP32" : "Conexión sin confirmar"}</span>
      </div>
      {queryError && <p className="admin-error" role="alert">{queryError}</p>}
      <div className="admin-control-row">
        <div>
          <h3>Redes cercanas al invernadero</h3>
          <p className="admin-help">El ESP32 escanea desde su ubicación. Las redes ocultas se introducen a mano.</p>
        </div>
        <div className="admin-mode-buttons"><button type="button" className="admin-option" disabled={disabled} onClick={() => void send({ kind: "scan" })}>Buscar redes</button></div>
        {networks && networks.length > 0 && <ul className="admin-wifi-networks" aria-label="Redes del último escaneo">
          {networks.map((network, index) => <li key={`${index}-${network.ssid}`}>
            <button type="button" className="admin-option" disabled={locked} aria-pressed={ssid === network.ssid} onClick={() => selectSsid(network.ssid)}>
              <bdi>{network.ssid}</bdi><span>{network.rssi} dBm · {network.secured ? "Protegida" : "Abierta"}</span>
            </button>
          </li>)}
        </ul>}
      </div>
      <div className="admin-control-row">
        <div>
          <h3>Cambiar de red</h3>
          <p className="admin-help" id="admin-wifi-warning"><strong>El ESP32 puede desconectarse temporalmente.</strong> Solo guarda la nueva red si logra comunicarse con el servidor desde ella.</p>
        </div>
        <form className="admin-wifi-form" onSubmit={connect} aria-describedby="admin-wifi-warning">
          <label htmlFor="admin-wifi-ssid">Nombre de red (SSID)</label>
          <input id="admin-wifi-ssid" type="text" value={ssid} onChange={(event) => selectSsid(event.target.value)} autoComplete="off" autoCapitalize="none" spellCheck={false} required disabled={locked} />
          <label htmlFor="admin-wifi-password">Contraseña (vacía si la red es abierta)</label>
          <input id="admin-wifi-password" ref={passwordField} type="password" autoComplete="new-password" disabled={locked} />
          <button className="admin-button" type="submit" disabled={disabled}>Solicitar cambio de Wi-Fi</button>
        </form>
      </div>
      <div className="admin-feedback" aria-live="polite" aria-atomic="true">
        {sending ? <p className="admin-help">Enviando solicitud Wi-Fi…</p> : state?.job && <p className={state.job.status === "pending" ? "admin-pending" : "admin-help"}>{jobMessage(state.job)}</p>}
        {uncertain && <p className="admin-pending">Los mandos Wi-Fi siguen bloqueados hasta que pase el plazo de la solicitud.</p>}
        {actionError && <p className="admin-error">{actionError}</p>}
      </div>
    </section>
  );
}

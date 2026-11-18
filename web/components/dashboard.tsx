"use client";

import { useId, useState } from "react";
import { COOLING_OFF_C, COOLING_ON_C, STALE_AFTER_MS, sampleAgeMs } from "../lib/types";
import { ClimateChart } from "./climate-chart";
import { useTelemetry } from "./use-telemetry";

function Info({ children, label }: { children: React.ReactNode; label: string }) {
  const id = useId();
  const [dismissed, setDismissed] = useState(false);
  return (
    <span className="info-wrap" onMouseEnter={() => setDismissed(false)}>
      <button type="button" className="info-button" aria-label={label} aria-describedby={id} onFocus={() => setDismissed(false)} onKeyDown={(event) => { if (event.key === "Escape") setDismissed(true); }}>i</button>
      <span className="info-tooltip" role="tooltip" id={id} style={dismissed ? { display: "none" } : undefined}>{children}</span>
    </span>
  );
}

export function Dashboard() {
  const { state, points, loading, queryError, configError, realtime, now, refresh } = useTelemetry();
  const age = state && now !== null ? sampleAgeMs(state, now) : null;
  const stale = age !== null && age > STALE_AFTER_MS;
  const sensorOk = state?.sensor_status === "ok";
  const hasValue = sensorOk && state.temperature_c !== null && state.humidity_pct !== null;
  const sensorLabel = !state ? "Sin lectura" : sensorOk ? "Lectura válida" : state.sensor_status === "out_of_range" ? "Sensor fuera de rango" : "Fallo de lectura del sensor";
  const headlineStatus = configError ? "Configuración pendiente" : loading ? "Consultando lecturas" : queryError ? "Consulta interrumpida" : !state ? "Sin primera lectura" : stale ? "Lectura desactualizada" : !sensorOk ? "Sensor con fallo" : "Lectura reciente";
  const recent = Boolean(state && !stale && !queryError && !configError && sensorOk);
  const measuredAt = state ? new Date(Date.parse(state.received_at) - state.sample_age_ms) : null;
  const dateText = measuredAt && now !== null ? new Intl.DateTimeFormat("es", { day: "numeric", month: "short", hour: "2-digit", minute: "2-digit", second: "2-digit", timeZoneName: "short" }).format(measuredAt) : null;
  const ageText = age === null ? null : age < 1_000 ? "hace menos de un segundo" : age < 60_000 ? `hace ${Math.floor(age / 1_000)} s` : age < 3_600_000 ? `hace ${Math.floor(age / 60_000)} min` : `hace ${Math.floor(age / 3_600_000)} h`;

  return (
    <>
      <a className="skip-link" href="#clima">Saltar a las lecturas</a>
      <div className="site-shell">
        <header className="site-header">
          <a className="identity" href="#clima" aria-label="Invernadero ESP32, inicio">
            <svg className="brand-mark" viewBox="0 0 40 44" aria-hidden="true"><path d="M5 38V17L20 5L35 17V38H5ZM20 5V38M5 17H35M20 30C11 30 10 24 10 24C17 23 20 27 20 30ZM20 25C20 20 25 18 29 19C29 24 25 27 20 27" /></svg>
            <span>Invernadero <span className="identity-model">ESP32</span></span>
          </a>
          <div className="header-status"><span className={`status-dot ${recent ? "is-live" : ""}`} aria-hidden="true" /><span>{headlineStatus}</span></div>
        </header>

        <main id="clima">
          <div className="intro">
            <div><p className="intro-kicker">Una ventana al cultivo</p><h1>Lo que se respira<br />dentro del invernadero.</h1></div>
            <p className="intro-note">Temperatura y humedad ambiental,<br className="desktop-break" /> medidas por un DHT11.<br /><span>Solo observar. Sin actuar sobre las cargas.</span></p>
          </div>

          <div className="notices" aria-live="polite" aria-atomic="true">
            {configError && <div className="notice"><strong>No se puede conectar con los datos.</strong><p>{configError} Configura las variables públicas y vuelve a desplegar la web.</p></div>}
            {queryError && <div className="notice"><div><strong>No se pudo consultar el estado actual.</strong><p>{queryError} {state ? "Se conserva la última lectura recibida; no es una confirmación del estado actual." : "No se ha podido comprobar si el dispositivo ha enviado lecturas."}</p></div><button type="button" className="text-button" onClick={refresh}>Reintentar consulta</button></div>}
            {!configError && realtime === "interrupted" && <div className="notice"><strong>La conexión en directo del navegador se ha interrumpido.</strong><p>Realtime intentará reconectar y consultará el estado al recuperarse. Esto no confirma una desconexión del ESP32.</p></div>}
            {stale && <div className="notice"><strong>El dispositivo no ha enviado una lectura reciente.</strong><p>Última muestra {ageText}. Puede haber una interrupción de Wi-Fi, Internet o del propio ESP32. Los valores que ves son antiguos.</p></div>}
            {state && !sensorOk && <div className="notice"><strong>{sensorLabel}.</strong><p>{state.sensor_status === "out_of_range" ? "La muestra está fuera del rango admitido. No se presenta como una medición válida." : "El DHT11 no produjo una lectura válida. Temperatura y humedad se muestran sin valor."}</p></div>}
            {state && (!state.outputs_safe || Object.values(state.gpio_levels).some((value) => value !== 0)) && <div className="notice"><strong>Salidas reportadas fuera del estado seguro esperado.</strong><p>Esta fase requiere todas las salidas LOW. La web no puede apagar ni verificar físicamente las cargas.</p></div>}
          </div>

          <div className="climate-layout">
            <section className={`greenhouse-section ${stale ? "is-stale" : ""}`} aria-labelledby="current-title" aria-busy={loading}>
              <div className="section-heading"><h2 id="current-title">El ambiente ahora</h2><span className="quiet-label">DHT11 <Info label="Qué mide el DHT11">Mide el aire junto al sensor, no la humedad del suelo. Una lectura no describe todo el cultivo.</Info></span></div>
              <div className="greenhouse-drawing">
                <svg className="greenhouse-lines" viewBox="0 0 620 430" preserveAspectRatio="none" aria-hidden="true">
                  <path className="greenhouse-roof" d="M40 168L310 32L580 168M66 155V389H554V155M310 32V389M120 129L363 253M500 129L257 253M66 207H554" />
                  <path className="greenhouse-base" d="M20 390H600M94 405H526" />
                  <path className="plant-lines" d="M151 388V347M151 367C126 368 120 354 119 345C136 344 150 351 151 367ZM152 358C155 341 165 335 182 336C180 349 170 359 152 358ZM459 388V330M459 355C439 354 430 344 429 333C445 332 457 340 459 355ZM459 344C460 324 471 316 488 317C488 333 477 344 459 344" />
                </svg>
                <div className="roof-label">Monitorización</div>
                <div className="climate-readings">
                  <div className="temperature-reading"><span className="metric-name">Temperatura</span><p className={`metric-value ${loading ? "is-loading" : ""}`}>{hasValue ? state.temperature_c!.toLocaleString("es", { maximumFractionDigits: 1, minimumFractionDigits: 1 }) : "—"}<span>°C</span></p></div>
                  <div className="humidity-reading"><span className="metric-name">Humedad relativa</span><p className={`metric-value ${loading ? "is-loading" : ""}`}>{hasValue ? state.humidity_pct!.toLocaleString("es", { maximumFractionDigits: 1, minimumFractionDigits: 1 }) : "—"}<span>%</span></p></div>
                </div>
                <div className="reading-quality">{loading ? "Esperando la consulta inicial" : !state ? "Sin mediciones disponibles" : stale ? "Última lectura · desactualizada" : sensorLabel}</div>
              </div>
              <div className="reading-caption">
                {dateText && measuredAt ? <><time dateTime={measuredAt.toISOString()}>{dateText}</time><span>{ageText}</span></> : <span>{loading ? "Conectando con el invernadero…" : configError || queryError ? "Sin datos confirmados. Consulta el aviso de conexión." : "Todavía no se ha recibido la primera muestra del dispositivo."}</span>}
              </div>
            </section>
            <ClimateChart points={points} />
          </div>

          <section className="system-section" aria-labelledby="system-title">
            <div className="system-heading"><h2 id="system-title">Lo que reporta el sistema</h2><p>Información del ESP32, no confirmación física de las cargas. {stale && <strong>Reporte desactualizado.</strong>}</p></div>
            <dl className="system-strip">
              <div><dt>Modo actual</dt><dd>Monitorización</dd><p>No se envían órdenes</p></div>
              <div><dt>Peltier <Info label="Por qué la Peltier está bloqueada">Protección térmica pendiente. Este canal no debe activarse en esta fase.</Info></dt><dd className="warning-text">Bloqueada</dd><p>{state && !state.peltier_locked ? "Alerta: bloqueo no confirmado" : "Protección térmica pendiente"}</p></div>
              <div><dt>Bomba · Q2</dt><dd>{state ? state.gpio_levels.pump === 0 ? "GPIO LOW" : "GPIO HIGH · revisar" : "Sin reporte"}</dd><p>El GPIO no confirma caudal</p></div>
              <div><dt>Ventiladores · Q3</dt><dd>No instalados</dd><p>{state ? `GPIO reportado: ${state.gpio_levels.fans === 0 ? "LOW" : "HIGH · revisar"}` : "Sin reporte del GPIO"}</p></div>
            </dl>
            <div className="diagnostics">
              <span>Wi-Fi del ESP32: <strong>{!state ? "sin reporte" : stale ? "sin confirmación reciente" : state.wifi_connected ? "conectado al muestrear" : "desconectado al muestrear"}</strong></span>
              <span>Memoria: <strong>{!state ? "sin reporte" : state.memory_config_ok ? "configuración reportada correcta" : "configuración incorrecta"}</strong></span>
              <span>Peltier · Q1: <strong>{state ? state.gpio_levels.peltier === 0 ? "GPIO LOW" : "GPIO HIGH · revisar" : "sin reporte"}</strong></span>
              <span>Realtime web: <strong>{configError ? "sin configurar" : realtime === "connected" ? "suscripción activa" : realtime === "interrupted" ? "interrumpido" : "conectando"}</strong></span>
            </div>
          </section>

          <aside className="context-section" aria-label="Alcance de la monitorización">
            <div className="context-leaf" aria-hidden="true"><svg viewBox="0 0 52 64"><path d="M11 56C15 37 31 24 43 8C50 40 33 52 16 49M13 52L28 37M26 39V26" /></svg></div>
            <div><h2>Observar primero. Cuidar con criterio.</h2><p>Los {COOLING_ON_C} °C de encendido y {COOLING_OFF_C} °C de apagado son umbrales previstos para un futuro automático, no un objetivo activo ni una recomendación agronómica. La fase actual solo mide.</p></div>
            <p className="future-note">El acceso administrador y los controles pertenecen a una fase futura. No hay actuación remota habilitada.</p>
          </aside>
        </main>
        <footer className="site-footer"><span>Invernadero ESP32</span><span>Datos reales. Sin histórico entre visitas.</span></footer>
      </div>
    </>
  );
}
